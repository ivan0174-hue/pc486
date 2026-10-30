//
// pc486 - Raspberry Pi 4 bare-metal front end (Circle)
//
// Runs the 486 PC emulator directly on the hardware:
//   - video: one HDMI frame buffer, emulated screen scaled by core 1
//   - input: USB keyboard and mouse (any number of hubs)
//   - storage: files on the FAT32 SD card in the folder /pc486
//   - sound: 3.5mm jack (PWM) or HDMI
//
#include "kernel.h"
#include <circle/bcmpropertytags.h>
#include <circle/string.h>
#include <circle/util.h>
#include <circle/sound/pwmsoundbasedevice.h>
#include <circle/sound/hdmisoundbasedevice.h>
#include <circle/alloc.h>
#include <stdio.h>

extern "C" {
#include "pc.h"
#include "config.h"
#include "keymap_hid.h"
#include "osd.h"
}

#define DRIVE		"SD:"
#define BASEDIR		"SD:/pc486"
#define INIFILE		BASEDIR "/pc486.ini"

static const char FromKernel[] = "pc486";

CKernel *CKernel::s_pThis = 0;

// ---------------------------------------------------------------------------
// event queue from USB interrupt context to the emulation loop

struct TInputEvent
{
	u8	nType;		// 0 = keyboard report, 1 = mouse
	u8	ucModifiers;
	u8	RawKeys[6];
	int	nDX, nDY, nWheel;
	unsigned nButtons;
};

#define EVQ_SIZE 256
static TInputEvent s_EventQueue[EVQ_SIZE];
static volatile unsigned s_nEvHead, s_nEvTail;

static void PushEvent (const TInputEvent &rEvent)
{
	unsigned nNext = (s_nEvTail + 1) % EVQ_SIZE;
	if (nNext == s_nEvHead)
	{
		return;		// queue full: drop
	}
	s_EventQueue[s_nEvTail] = rEvent;
	DataMemBarrier ();
	s_nEvTail = nNext;
}

static boolean PopEvent (TInputEvent *pEvent)
{
	if (s_nEvHead == s_nEvTail)
	{
		return FALSE;
	}
	DataMemBarrier ();
	*pEvent = s_EventQueue[s_nEvHead];
	s_nEvHead = (s_nEvHead + 1) % EVQ_SIZE;
	return TRUE;
}

// ---------------------------------------------------------------------------
// C platform layer used by the emulator core

static PC *s_pPC = 0;
static PCConfig s_Config;
static CEMMCDevice *s_pEMMC = 0;

extern "C" {

uint32_t get_uticks (void)
{
	return CTimer::GetClockTicks ();
}

void *bigmalloc (size_t nSize)
{
	return malloc (nSize);
}

long plat_load_file (const char *pPath, void *pBuffer, long nMaxLen)
{
	FIL File;
	if (f_open (&File, pPath, FA_READ | FA_OPEN_EXISTING) != FR_OK)
	{
		return -1;
	}
	UINT nRead = 0;
	FRESULT Result = f_read (&File, pBuffer, nMaxLen, &nRead);
	f_close (&File);
	return Result == FR_OK ? (long) nRead : -1;
}

int plat_save_file (const char *pPath, const void *pBuffer, long nLen)
{
	FIL File;
	if (f_open (&File, pPath, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		return -1;
	}
	UINT nWritten = 0;
	f_write (&File, pBuffer, nLen, &nWritten);
	f_close (&File);
	return nWritten == (UINT) nLen ? 0 : -1;
}

void plat_log (const char *pFormat, ...)
{
	char Buffer[256];
	va_list Args;
	va_start (Args, pFormat);
	vsnprintf (Buffer, sizeof Buffer, pFormat, Args);
	va_end (Args);
	size_t n = strlen (Buffer);
	if (n > 0 && Buffer[n-1] == '\n')
	{
		Buffer[n-1] = '\0';
	}
	CLogger::Get ()->Write (FromKernel, LogNotice, "%s", Buffer);
}

// raw sectors of the SD card (used for "hda = sd:2" style disks)
static u8 s_SectorBuffer[512] __attribute__ ((aligned (64)));

int plat_raw_read (uint64_t nLBA, void *pBuffer, int nCount)
{
	if (s_pEMMC == 0)
	{
		return -1;
	}
	u8 *p = (u8 *) pBuffer;
	for (int i = 0; i < nCount; i++)
	{
		if (   s_pEMMC->Seek ((nLBA + i) * 512) != (nLBA + i) * 512
		    || s_pEMMC->Read (s_SectorBuffer, 512) != 512)
		{
			return -1;
		}
		memcpy (p + i * 512, s_SectorBuffer, 512);
	}
	return 0;
}

int plat_raw_write (uint64_t nLBA, const void *pBuffer, int nCount)
{
	if (s_pEMMC == 0)
	{
		return -1;
	}
	const u8 *p = (const u8 *) pBuffer;
	for (int i = 0; i < nCount; i++)
	{
		memcpy (s_SectorBuffer, p + i * 512, 512);
		if (   s_pEMMC->Seek ((nLBA + i) * 512) != (nLBA + i) * 512
		    || s_pEMMC->Write (s_SectorBuffer, 512) != 512)
		{
			return -1;
		}
	}
	return 0;
}

int plat_list_dir (const char *pDir, char Names[][64], int nMax)
{
	DIR Directory;
	FILINFO FileInfo;
	int n = 0;
	if (f_opendir (&Directory, pDir) != FR_OK)
	{
		return 0;
	}
	while (n < nMax && f_readdir (&Directory, &FileInfo) == FR_OK && FileInfo.fname[0])
	{
		if (FileInfo.fattrib & (AM_DIR | AM_HID | AM_SYS))
		{
			continue;
		}
		strncpy (Names[n], FileInfo.fname, 63);
		Names[n][63] = '\0';
		n++;
	}
	f_closedir (&Directory);
	return n;
}

void pc486_console_write (const char *pString, int nLength)
{
	static char Line[256];
	static int nPos = 0;
	for (int i = 0; i < nLength; i++)
	{
		char c = pString[i];
		if (c == '\n' || nPos >= (int) sizeof Line - 1)
		{
			Line[nPos] = '\0';
			CLogger::Get ()->Write (FromKernel, LogDebug, "%s", Line);
			nPos = 0;
		}
		else
		{
			Line[nPos++] = c;
		}
	}
}

void pc486_fatal (const char *pMessage)
{
	CLogger::Get ()->Write (FromKernel, LogPanic, "%s", pMessage);
	for (;;);
}

}	// extern "C"

// ---------------------------------------------------------------------------

CKernel::CKernel (void)
:	m_BootLog (&m_Serial),
	m_Timer (&m_Interrupt),
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_USBHCI (&m_Interrupt, &m_Timer, TRUE),
	m_EMMC (&m_Interrupt, &m_Timer, &m_ActLED),
	m_bFileSystem (FALSE),
	m_pFrameBuffer (0),
	m_nDisplayWidth (0),
	m_nDisplayHeight (0),
	m_nAspectMode (1),
	m_pKeyboard (0),
	m_pMouse (0),
	m_pSound (0),
	m_pSound2 (0),
	m_VideoCore (this),
	m_bVideoRunning (FALSE),
	m_nVideoFrames (0),
	m_bVideoOnCore0 (FALSE)
{
	s_pThis = this;
	s_pEMMC = &m_EMMC;
	m_ActLED.Blink (3);
}

CKernel::~CKernel (void)
{
	s_pThis = 0;
}

boolean CKernel::Initialize (void)
{
	m_Serial.Initialize (115200);

	// physical display size: the frame buffer gets the same aspect ratio,
	// so the GPU scales it without distortion
	{
		CBcmPropertyTags Tags;
		TPropertyTagDisplayDimensions Dimensions;
		if (Tags.GetTag (PROPTAG_GET_DISPLAY_DIMENSIONS, &Dimensions, sizeof Dimensions))
		{
			m_nDisplayWidth = Dimensions.nWidth;
			m_nDisplayHeight = Dimensions.nHeight;
		}
	}
	unsigned nFBHeight = 768;
	unsigned nFBWidth = 1024;
	if (   m_nDisplayWidth >= 320 && m_nDisplayHeight >= 200
	    && m_nDisplayWidth <= 8192 && m_nDisplayHeight <= 4096)
	{
		nFBWidth = (nFBHeight * m_nDisplayWidth / m_nDisplayHeight + 7) & ~7;
		if (nFBWidth < 1024)
		{
			nFBWidth = 1024;
		}
		if (nFBWidth > 2048)
		{
			nFBWidth = 2048;
		}
	}
	if (m_BootLog.InitScreen (nFBWidth, nFBHeight))
	{
		m_pFrameBuffer = m_BootLog.GetFrameBuffer ();
	}

	if (!m_Logger.Initialize (&m_BootLog))
	{
		return FALSE;
	}
	m_Logger.Write (FromKernel, LogNotice, "pc486 - versione del " __DATE__ " " __TIME__);
	m_Logger.Write (FromKernel, LogNotice, "Schermo %ux%u, frame buffer %ux%u %s",
			m_nDisplayWidth, m_nDisplayHeight, nFBWidth, nFBHeight,
			m_pFrameBuffer != 0 ? "OK" : "NON DISPONIBILE");

	m_Logger.Write (FromKernel, LogNotice, "Avvio interrupt...");
	if (!m_Interrupt.Initialize ())
	{
		Fatal ("Errore: interrupt");
	}

	m_Logger.Write (FromKernel, LogNotice, "Avvio timer...");
	if (!m_Timer.Initialize ())
	{
		Fatal ("Errore: timer");
	}

	m_Logger.Write (FromKernel, LogNotice, "Avvio scheda SD...");
	if (!m_EMMC.Initialize ())
	{
		Fatal ("Errore: scheda SD non inizializzata");
	}

	m_Logger.Write (FromKernel, LogNotice, "Avvio USB...");
	if (!m_USBHCI.Initialize ())
	{
		m_Logger.Write (FromKernel, LogError, "Errore: USB non disponibile (continuo senza tastiera)");
	}

	m_Logger.Write (FromKernel, LogNotice, "Avvio core 1 (video)...");
	if (!m_VideoCore.Initialize ())		// must be last: starts cores 1-3
	{
		m_Logger.Write (FromKernel, LogError, "Errore: multicore, il video usera' il core 0");
		m_bVideoOnCore0 = TRUE;
	}

	return TRUE;
}

void CKernel::Fatal (const char *pMessage)
{
	m_Logger.Write (FromKernel, LogError, "%s", pMessage);
	m_Logger.Write (FromKernel, LogError, "Sistema fermo. Il log e' in pc486/log.txt (se la SD funziona).");
	SaveLog ();
	for (;;)
	{
		m_ActLED.On ();
		CTimer::SimpleMsDelay (200);
		m_ActLED.Off ();
		CTimer::SimpleMsDelay (800);
	}
}

void CKernel::SaveLog (void)
{
	if (!m_bFileSystem || !m_BootLog.TakeDirty ())
	{
		return;
	}
	FIL File;
	if (f_open (&File, BASEDIR "/log.txt", FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
	{
		return;
	}
	UINT nWritten;
	f_write (&File, m_BootLog.GetText (), m_BootLog.GetLength (), &nWritten);
	f_close (&File);
}

void CVideoCore::Run (unsigned nCore)
{
	if (nCore == 1)
	{
		m_pKernel->VideoLoop ();
	}
}

void CKernel::VideoLoop (void)
{
	while (!m_bVideoRunning)
	{
		CTimer::SimpleusDelay (1000);
	}
	m_nVideoFrames = 1;	// tell core 0 that this core is alive

	// leave the boot messages on screen for a moment
	CTimer::SimpleMsDelay (1500);

	unsigned nLastFrame = CTimer::GetClockTicks ();
	for (;;)
	{
		unsigned nNow = CTimer::GetClockTicks ();
		if (nNow - nLastFrame < 16667)
		{
			continue;
		}
		nLastFrame = nNow;
		Present ();
		m_nVideoFrames++;
	}
}

// ---------------------------------------------------------------------------
// video: render the emulated screen and scale it into the frame buffer,
// writing only the lines that changed (the frame buffer is uncached
// device memory, so every store is expensive)

static u32 *s_pRender, *s_pPrev;
static int s_nPrevW = -1, s_nPrevH = -1;
static u16 s_XMap[2048];
static u8 s_RowDirty[S3V_MAX_H];
static boolean s_bFirstFrame = TRUE;

void CKernel::Present (void)
{
	if (m_pFrameBuffer == 0 || s_pPC == 0)
	{
		return;
	}

	int w, h, nPitch;
	s3v_render (s_pPC->vga, s_pRender, &w, &h, &nPitch);
	if (osd_visible ())
	{
		osd_draw (s_pRender, nPitch, w, h);
	}
	if (w < 1 || h < 1)
	{
		return;
	}

	if (s_bFirstFrame)
	{
		s_bFirstFrame = FALSE;
		m_BootLog.ReleaseScreen ();
		m_Logger.Write (FromKernel, LogNotice, "Primo fotogramma %dx%d", w, h);
	}

	volatile u32 *pFB = (volatile u32 *) (uintptr) m_pFrameBuffer->GetBuffer ();
	unsigned nFBPitch = m_pFrameBuffer->GetPitch () / 4;
	int nFBW = m_pFrameBuffer->GetWidth ();
	int nFBH = m_pFrameBuffer->GetHeight ();

	// target area
	int aw = nFBW, ah = nFBH;
	if (m_nAspectMode == 1 && nFBH * 4 / 3 <= nFBW)
	{
		aw = nFBH * 4 / 3;
	}
	int x0 = (nFBW - aw) / 2;

	boolean bFull = FALSE;
	if (w != s_nPrevW || h != s_nPrevH)
	{
		bFull = TRUE;
		s_nPrevW = w;
		s_nPrevH = h;
		for (int y = 0; y < nFBH; y++)
		{
			for (int x = 0; x < nFBW; x++)
			{
				pFB[y * nFBPitch + x] = 0xFF000000;
			}
		}
		for (int x = 0; x < aw && x < 2048; x++)
		{
			s_XMap[x] = (u16) (x * w / aw);
		}
		m_Logger.Write (FromKernel, LogNotice, "Modo video %dx%d (%s)", w, h,
				s3v_mode_desc (s_pPC->vga));
	}

	for (int y = 0; y < h; y++)
	{
		u32 *pS = &s_pRender[y * nPitch];
		u32 *pP = &s_pPrev[y * nPitch];
		if (bFull || memcmp (pS, pP, w * 4) != 0)
		{
			memcpy (pP, pS, w * 4);
			s_RowDirty[y] = 1;
		}
		else
		{
			s_RowDirty[y] = 0;
		}
	}

	for (int dy = 0; dy < ah; dy++)
	{
		int sy = dy * h / ah;
		if (!s_RowDirty[sy])
		{
			continue;
		}
		const u32 *pS = &s_pPrev[sy * nPitch];
		volatile u32 *pD = pFB + dy * nFBPitch + x0;
		for (int dx = 0; dx < aw; dx++)
		{
			pD[dx] = pS[s_XMap[dx]] | 0xFF000000;
		}
	}
}

// ---------------------------------------------------------------------------
// USB input

void CKernel::KeyStatusHandlerRaw (unsigned char ucModifiers, const unsigned char RawKeys[6])
{
	TInputEvent Event;
	Event.nType = 0;
	Event.ucModifiers = ucModifiers;
	memcpy (Event.RawKeys, RawKeys, 6);
	PushEvent (Event);
}

void CKernel::MouseStatusHandler (unsigned nButtons, int nDisplacementX, int nDisplacementY, int nWheelMove)
{
	TInputEvent Event;
	Event.nType = 1;
	Event.nButtons = nButtons;
	Event.nDX = nDisplacementX;
	Event.nDY = nDisplacementY;
	Event.nWheel = nWheelMove;
	PushEvent (Event);
}

void CKernel::KeyboardRemovedHandler (CDevice *pDevice, void *pContext)
{
	s_pThis->m_pKeyboard = 0;
}

void CKernel::MouseRemovedHandler (CDevice *pDevice, void *pContext)
{
	s_pThis->m_pMouse = 0;
}

static HidKbdState s_KbdState;

static void EmitKey (void *pOpaque, int bDown, int nSet1)
{
	if (osd_active () || osd_hotkey (bDown, nSet1))
	{
		if (osd_active ())
		{
			osd_key (s_pPC, bDown, nSet1);
		}
		return;
	}
	pc_key_event ((PC *) pOpaque, bDown, nSet1);
}

void CKernel::PollDevices (void)
{
	static unsigned nCount = 0;
	if (nCount++ % 25 == 0 && m_USBHCI.UpdatePlugAndPlay ())
	{
		if (m_pKeyboard == 0)
		{
			m_pKeyboard = (CUSBKeyboardDevice *) m_DeviceNameService.GetDevice ("ukbd1", FALSE);
			if (m_pKeyboard != 0)
			{
				m_pKeyboard->RegisterRemovedHandler (KeyboardRemovedHandler);
				m_pKeyboard->RegisterKeyStatusHandlerRaw (KeyStatusHandlerRaw);
				m_Logger.Write (FromKernel, LogNotice, "Tastiera collegata");
			}
		}

		if (m_pMouse == 0)
		{
			m_pMouse = (CMouseDevice *) m_DeviceNameService.GetDevice ("mouse1", FALSE);
			if (m_pMouse != 0)
			{
				m_pMouse->RegisterRemovedHandler (MouseRemovedHandler);
				m_pMouse->RegisterStatusHandler (MouseStatusHandler);
				m_Logger.Write (FromKernel, LogNotice, "Mouse collegato");
			}
		}
	}

	TInputEvent Event;
	while (PopEvent (&Event))
	{
		if (Event.nType == 0)
		{
			hid_report_diff (&s_KbdState, Event.ucModifiers, Event.RawKeys, EmitKey, s_pPC);
		}
		else if (!osd_active ())
		{
			pc_mouse_event (s_pPC, Event.nDX, Event.nDY, Event.nWheel, Event.nButtons & 7);
		}
	}
}

// ---------------------------------------------------------------------------
// sound

void CKernel::StartSound (void)
{
	if (!s_Config.sound)
	{
		return;
	}
	const char *pOut = osd_get_option ("sound_out");
	if (pOut != 0 && strcmp (pOut, "none") == 0)
	{
		return;
	}
	// default: both the 3.5 mm jack and HDMI (TV / monitor speakers)
	boolean bHDMI = pOut == 0 || strcmp (pOut, "pwm") != 0;
	boolean bPWM = pOut == 0 || strcmp (pOut, "hdmi") != 0;

	CSoundBaseDevice *pDev[2] = {0, 0};
	const char *pName[2] = {"HDMI", "jack 3.5 mm"};
	if (bHDMI)
	{
		pDev[0] = new CHDMISoundBaseDevice (&m_Interrupt, 44100, 384 * 10);
	}
	if (bPWM)
	{
		pDev[1] = new CPWMSoundBaseDevice (&m_Interrupt, 44100, 2048);
	}
	for (unsigned i = 0; i < 2; i++)
	{
		if (pDev[i] == 0)
		{
			continue;
		}
		if (   !pDev[i]->AllocateQueue (150)
		    || (pDev[i]->SetWriteFormat (SoundFormatSigned16, 2), !pDev[i]->Start ()))
		{
			m_Logger.Write (FromKernel, LogWarning, "Audio %s non disponibile", pName[i]);
			delete pDev[i];
			continue;
		}
		m_Logger.Write (FromKernel, LogNotice, "Audio avviato: %s", pName[i]);
		if (m_pSound == 0)
		{
			m_pSound = pDev[i];
		}
		else
		{
			m_pSound2 = pDev[i];
		}
	}
}

// ---------------------------------------------------------------------------

TShutdownMode CKernel::Run (void)
{
	m_CPUThrottle.SetSpeed (CPUSpeedMaximum);
	m_Logger.Write (FromKernel, LogNotice, "CPU a %u MHz", m_CPUThrottle.GetClockRate () / 1000000);

	m_Logger.Write (FromKernel, LogNotice, "Lettura scheda SD (FAT32)...");
	if (f_mount (&m_FileSystem, DRIVE, 1) != FR_OK)
	{
		Fatal ("Errore: impossibile leggere la scheda SD (deve essere FAT32).");
	}
	m_bFileSystem = TRUE;
	SaveLog ();

	pc_config_defaults (&s_Config);
	s_Config.bios = BASEDIR "/ami486.bin";
	s_Config.vga_bios = BASEDIR "/s3virge.bin";
	s_Config.nvram = BASEDIR "/cmos.bin";
	osd_set_basedir (BASEDIR);
	if (pc_config_load (&s_Config, INIFILE, BASEDIR) < 0)
	{
		m_Logger.Write (FromKernel, LogWarning, "Manca %s: uso i valori predefiniti", INIFILE);
	}
	else
	{
		m_Logger.Write (FromKernel, LogNotice, "Configurazione letta: RAM %ld MB, disco C: %s",
				s_Config.mem_size >> 20,
				s_Config.hd[0] != 0 && s_Config.hd[0][0] ? s_Config.hd[0] : "-");
	}
	osd_load_options (INIFILE);
	const char *pAspect = osd_get_option ("aspect");
	if (pAspect != 0 && strcmp (pAspect, "stretch") == 0)
	{
		m_nAspectMode = 0;
	}

	s_pRender = (u32 *) malloc (S3V_MAX_W * S3V_MAX_H * 4);
	s_pPrev = (u32 *) malloc (S3V_MAX_W * S3V_MAX_H * 4);
	if (s_pRender == 0 || s_pPrev == 0)
	{
		Fatal ("Errore: memoria insufficiente");
	}

	m_Logger.Write (FromKernel, LogNotice, "Creo il PC emulato...");
	SaveLog ();
	s_pPC = pc_new (&s_Config);
	if (s_pPC == 0)
	{
		CString Msg;
		Msg.Format ("Errore: controlla che esistano %s e %s", s_Config.bios, s_Config.vga_bios);
		Fatal (Msg);
	}
	osd_init (s_pPC, &s_Config);
	m_Logger.Write (FromKernel, LogNotice, "PC pronto, avvio del BIOS");

	StartSound ();
	SaveLog ();

	m_bVideoRunning = TRUE;		// core 1 starts presenting frames

	unsigned nStart = CTimer::GetClockTicks ();
	unsigned nLastPoll = nStart;
	unsigned nLastThrottle = nStart;
	unsigned nLastAudio = nStart;
	unsigned nLastFrame = nStart;
	unsigned nLastStat = nStart;
	unsigned nLastLogSave = nStart;
	unsigned long nLastCycles = 0;
	unsigned nStatCount = 0;
	static u8 AudioBuffer[4096];

	for (unsigned nLoop = 0; ; nLoop++)
	{
		if (!osd_paused ())
		{
			pc_step (s_pPC);
		}

		if ((nLoop & 15) != 0)
		{
			continue;
		}

		unsigned nNow = CTimer::GetClockTicks ();

		if (nNow - nLastPoll >= 2000)
		{
			PollDevices ();
			nLastPoll = nNow;
		}

		if (m_pSound != 0 && nNow - nLastAudio >= 5000)
		{
			// keep about 80 ms of audio queued (the HDMI device takes
			// blocks of about 43 ms)
			unsigned nAvail = m_pSound->GetQueueFramesAvail ();
			unsigned nQueued = m_pSound->GetQueueSizeFrames () - nAvail;
			if (nQueued < 44100 * 80 / 1000)
			{
				unsigned nFrames = 44100 * 80 / 1000 - nQueued;
				if (nFrames > sizeof AudioBuffer / 4)
				{
					nFrames = sizeof AudioBuffer / 4;
				}
				pc_audio_callback (s_pPC, AudioBuffer, nFrames * 4);
				m_pSound->Write (AudioBuffer, nFrames * 4);
				if (m_pSound2 != 0)
				{
					m_pSound2->Write (AudioBuffer, nFrames * 4);
				}
			}
			nLastAudio = nNow;
		}

		// fall back to presenting on this core if core 1 does not run
		if (   !m_bVideoOnCore0
		    && m_nVideoFrames == 0
		    && nNow - nStart >= 3000000)
		{
			m_Logger.Write (FromKernel, LogWarning, "Il core 1 non risponde: video sul core 0");
			m_bVideoOnCore0 = TRUE;
		}
		if (m_bVideoOnCore0 && nNow - nLastFrame >= 33333)
		{
			Present ();
			nLastFrame = nNow;
		}

		if (osd_take_reboot_request ())
		{
			pc_save_nvram (s_pPC, 1);
			SaveLog ();
			return ShutdownReboot;
		}

		// statistics in the log (pc486/log.txt) during the first minutes
		if (nNow - nLastStat >= 5000000 && nStatCount < 24)
		{
			unsigned long nCycles = cpui386_get_cycle (s_pPC->cpu);
			m_Logger.Write (FromKernel, LogNotice, "POST %02X, %lu MIPS, video: %s, frame core1 %u",
					s_pPC->post_code, (nCycles - nLastCycles) / 5000000,
					s3v_mode_desc (s_pPC->vga), m_nVideoFrames);
			nLastCycles = nCycles;
			nLastStat = nNow;
			nStatCount++;
		}
		if (nNow - nLastLogSave >= 5000000)
		{
			SaveLog ();
			nLastLogSave = nNow;
		}

		if (nNow - nLastThrottle >= 1000000)
		{
			m_CPUThrottle.Update ();
			nLastThrottle = nNow;
		}
	}

	return ShutdownHalt;
}
