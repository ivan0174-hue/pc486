//
// pc486 - Raspberry Pi 4 bare-metal front end (Circle)
//
#ifndef _pc486_kernel_h
#define _pc486_kernel_h

#include <circle/actled.h>
#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#include <circle/serial.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/cputhrottle.h>
#include <circle/bcmframebuffer.h>
#include <circle/usb/usbhcidevice.h>
#include <circle/usb/usbkeyboard.h>
#include <circle/input/mouse.h>
#include <circle/sound/soundbasedevice.h>
#include <SDCard/emmc.h>
#include <fatfs/ff.h>
#include <circle/multicore.h>
#include <circle/memory.h>
#include <circle/types.h>
#include "bootlog.h"

enum TShutdownMode
{
	ShutdownNone,
	ShutdownHalt,
	ShutdownReboot
};

class CKernel;

// core 1 renders the emulated display while core 0 runs the emulation
class CVideoCore : public CMultiCoreSupport
{
public:
	CVideoCore (CKernel *pKernel)
	:	CMultiCoreSupport (CMemorySystem::Get ()),
		m_pKernel (pKernel)
	{
	}

	void Run (unsigned nCore);

private:
	CKernel *m_pKernel;
};

class CKernel
{
public:
	CKernel (void);
	~CKernel (void);

	boolean Initialize (void);

	TShutdownMode Run (void);

	void VideoLoop (void);		// runs on core 1

private:
	void Fatal (const char *pMessage);
	void SaveLog (void);
	void Present (void);
	void PollDevices (void);
	void StartSound (void);

	static void KeyStatusHandlerRaw (unsigned char ucModifiers, const unsigned char RawKeys[6]);
	static void MouseStatusHandler (unsigned nButtons, int nDisplacementX, int nDisplacementY, int nWheelMove);
	static void KeyboardRemovedHandler (CDevice *pDevice, void *pContext);
	static void MouseRemovedHandler (CDevice *pDevice, void *pContext);

private:
	// do not change this order
	CActLED			m_ActLED;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CSerialDevice		m_Serial;
	CBootLog		m_BootLog;
	CExceptionHandler	m_ExceptionHandler;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CLogger			m_Logger;
	CCPUThrottle		m_CPUThrottle;
	CUSBHCIDevice		m_USBHCI;
	CEMMCDevice		m_EMMC;
	FATFS			m_FileSystem;
	boolean			m_bFileSystem;

	CBcmFrameBuffer	       *m_pFrameBuffer;	// owned by m_BootLog
	unsigned		m_nDisplayWidth, m_nDisplayHeight;
	int			m_nAspectMode;		// 0 = stretch, 1 = 4:3

	CUSBKeyboardDevice * volatile m_pKeyboard;
	CMouseDevice * volatile m_pMouse;
	CSoundBaseDevice       *m_pSound;	// first output (sets the pace)
	CSoundBaseDevice       *m_pSound2;	// second output with sound_out = both
	CVideoCore		m_VideoCore;
	volatile boolean	m_bVideoRunning;
	volatile unsigned	m_nVideoFrames;
	boolean			m_bVideoOnCore0;

	static CKernel *s_pThis;
};

#endif
