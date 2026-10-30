//
// pc486 - boot log: logger target that writes to the serial port,
// keeps a copy in memory (saved to the SD card) and, while the system
// starts, shows the messages on the HDMI screen.
//
#ifndef _pc486_bootlog_h
#define _pc486_bootlog_h

#include <circle/device.h>
#include <circle/serial.h>
#include <circle/bcmframebuffer.h>
#include <circle/spinlock.h>
#include <circle/types.h>

class CBootLog : public CDevice
{
public:
	CBootLog (CSerialDevice *pSerial);
	~CBootLog (void);

	// creates the (only) frame buffer of the system
	boolean InitScreen (unsigned nWidth, unsigned nHeight);
	CBcmFrameBuffer *GetFrameBuffer (void) const	{ return m_pFrameBuffer; }
	// stop drawing (the emulator takes over the display)
	void ReleaseScreen (void);
	boolean HasScreen (void) const	{ return m_pFrameBuffer != 0; }

	int Write (const void *pBuffer, size_t nCount);

	// whole log text so far
	const char *GetText (void) const	{ return m_Text; }
	unsigned GetLength (void) const		{ return m_nTextLen; }
	boolean TakeDirty (void);

private:
	void PutChar (char c);
	void Redraw (void);

private:
	CSerialDevice *m_pSerial;
	CBcmFrameBuffer *m_pFrameBuffer;

	enum { COLS = 80, ROWS = 30 };
	char m_Screen[ROWS][COLS + 1];
	char m_Shown[ROWS][COLS];	// what is currently drawn
	unsigned m_nRow, m_nCol;

	enum { TEXT_MAX = 32768 };
	char *m_Text;
	unsigned m_nTextLen;
	volatile boolean m_bDirty;
	volatile boolean m_bScreenOff;

	CSpinLock m_SpinLock;
};

#endif
