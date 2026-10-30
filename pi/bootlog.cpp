//
// pc486 - boot log (see bootlog.h)
//
#include "bootlog.h"
#include <circle/util.h>

extern "C" const unsigned char vgafont16[256 * 16];

CBootLog::CBootLog (CSerialDevice *pSerial)
:	m_pSerial (pSerial),
	m_pFrameBuffer (0),
	m_nRow (0),
	m_nCol (0),
	m_nTextLen (0),
	m_bDirty (FALSE),
	m_bScreenOff (FALSE),
	m_SpinLock (IRQ_LEVEL)
{
	memset (m_Screen, ' ', sizeof m_Screen);
	for (unsigned i = 0; i < ROWS; i++)
	{
		m_Screen[i][COLS] = '\0';
	}
	memset (m_Shown, 0, sizeof m_Shown);
	m_Text = new char[TEXT_MAX];
	m_Text[0] = '\0';
}

CBootLog::~CBootLog (void)
{
}

boolean CBootLog::InitScreen (unsigned nWidth, unsigned nHeight)
{
	CBcmFrameBuffer *pFB = new CBcmFrameBuffer (nWidth, nHeight, 32);
	if (!pFB->Initialize ())
	{
		delete pFB;
		return FALSE;
	}

	// dark blue background, written with 32-bit stores only
	// (the frame buffer is device memory)
	volatile u32 *pBuffer = (volatile u32 *) (uintptr) pFB->GetBuffer ();
	unsigned nPitch = pFB->GetPitch () / 4;
	for (unsigned y = 0; y < pFB->GetHeight (); y++)
	{
		for (unsigned x = 0; x < pFB->GetWidth (); x++)
		{
			pBuffer[y * nPitch + x] = 0xFF000040;
		}
	}
	m_SpinLock.Acquire ();
	m_pFrameBuffer = pFB;
	m_SpinLock.Release ();
	Redraw ();
	return TRUE;
}

void CBootLog::ReleaseScreen (void)
{
	m_SpinLock.Acquire ();
	m_bScreenOff = TRUE;	// the emulator draws into the frame buffer from now on
	m_SpinLock.Release ();
}

boolean CBootLog::TakeDirty (void)
{
	boolean b = m_bDirty;
	m_bDirty = FALSE;
	return b;
}

void CBootLog::PutChar (char c)
{
	if (m_nTextLen < TEXT_MAX - 1)
	{
		m_Text[m_nTextLen++] = c;
		m_Text[m_nTextLen] = '\0';
	}

	if (c == '\r')
	{
		return;
	}
	if (c == '\n' || m_nCol >= COLS)
	{
		m_nCol = 0;
		if (++m_nRow >= ROWS)
		{
			// scroll
			for (unsigned i = 1; i < ROWS; i++)
			{
				memcpy (m_Screen[i-1], m_Screen[i], COLS);
			}
			memset (m_Screen[ROWS-1], ' ', COLS);
			m_nRow = ROWS-1;
		}
		if (c == '\n')
		{
			return;
		}
	}
	if ((unsigned char) c < ' ')
	{
		c = ' ';
	}
	m_Screen[m_nRow][m_nCol++] = c;
}

void CBootLog::Redraw (void)
{
	if (m_pFrameBuffer == 0 || m_bScreenOff)
	{
		return;
	}
	volatile u32 *pBuffer = (volatile u32 *) (uintptr) m_pFrameBuffer->GetBuffer ();
	unsigned nPitch = m_pFrameBuffer->GetPitch () / 4;
	unsigned nRows = m_pFrameBuffer->GetHeight () / 16;
	unsigned nCols = m_pFrameBuffer->GetWidth () / 8;
	for (unsigned r = 0; r < ROWS && r < nRows; r++)
	{
		if (memcmp (m_Shown[r], m_Screen[r], COLS) == 0)
		{
			continue;	// row unchanged
		}
		memcpy (m_Shown[r], m_Screen[r], COLS);
		for (unsigned c = 0; c < COLS && c < nCols; c++)
		{
			const unsigned char *pGlyph = &vgafont16[(unsigned char) m_Screen[r][c] * 16];
			for (unsigned y = 0; y < 16; y++)
			{
				volatile u32 *pLine = pBuffer + (r * 16 + y) * nPitch + c * 8;
				u8 b = pGlyph[y];
				for (unsigned x = 0; x < 8; x++)
				{
					pLine[x] = (b & (0x80 >> x)) ? 0xFFFFFFFF : 0xFF000040;
				}
			}
		}
	}
}

int CBootLog::Write (const void *pBuffer, size_t nCount)
{
	if (m_pSerial != 0)
	{
		m_pSerial->Write (pBuffer, nCount);
	}

	m_SpinLock.Acquire ();
	const char *p = (const char *) pBuffer;
	for (size_t i = 0; i < nCount; i++)
	{
		// skip ANSI escape sequences used by the logger for colours
		if (p[i] == '\x1b')
		{
			while (i < nCount && !((p[i] >= 'a' && p[i] <= 'z') || (p[i] >= 'A' && p[i] <= 'Z')))
			{
				i++;
			}
			continue;
		}
		PutChar (p[i]);
	}
	m_bDirty = TRUE;
	Redraw ();
	m_SpinLock.Release ();

	return (int) nCount;
}
