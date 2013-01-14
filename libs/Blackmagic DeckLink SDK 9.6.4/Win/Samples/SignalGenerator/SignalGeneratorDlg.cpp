/* -LICENSE-START-
** Copyright (c) 2009 Blackmagic Design
**
** Permission is hereby granted, free of charge, to any person or organization
** obtaining a copy of the software and accompanying documentation covered by
** this license (the "Software") to use, reproduce, display, distribute,
** execute, and transmit the Software, and to prepare derivative works of the
** Software, and to permit third-parties to whom the Software is furnished to
** do so, all subject to the following:
** 
** The copyright notices in the Software and this entire statement, including
** the above license grant, this restriction and the following disclaimer,
** must be included in all copies of the Software, in whole or in part, and
** all derivative works of the Software, unless such copies or derivative
** works are solely in the form of machine-executable object code generated by
** a source language processor.
** 
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO EVENT
** SHALL THE COPYRIGHT HOLDERS OR ANYONE DISTRIBUTING THE SOFTWARE BE LIABLE
** FOR ANY DAMAGES OR OTHER LIABILITY, WHETHER IN CONTRACT, TORT OR OTHERWISE,
** ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
** -LICENSE-END-
*/
// SignalGeneratorDlg.cpp : implementation file
//

#include "stdafx.h"
#include "SignalGenerator.h"
#include "SignalGeneratorDlg.h"
#define _USE_MATH_DEFINES
#include <math.h>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

const unsigned long		kAudioWaterlevel = 48000;

// SD 75% Colour Bars
static DWORD gSD75pcColourBars[8] =
{
	0xeb80eb80, 0xa28ea22c, 0x832c839c, 0x703a7048,
	0x54c654b8, 0x41d44164, 0x237223d4, 0x10801080
};

// HD 75% Colour Bars
static DWORD gHD75pcColourBars[8] =
{
	0xeb80eb80, 0xa888a82c, 0x912c9193, 0x8534853f,
	0x3fcc3fc1, 0x33d4336d, 0x1c781cd4, 0x10801080
};


CSignalGeneratorDlg::CSignalGeneratorDlg(CWnd* pParent /*=NULL*/)
	: CDialog(CSignalGeneratorDlg::IDD, pParent)
{
	m_hIcon = AfxGetApp()->LoadIcon(IDR_MAINFRAME);
	
	// Initialize instance variables
	m_running = false;
	m_deckLink = NULL;
	m_deckLinkOutput = NULL;
	
	m_videoFrameBlack = NULL;
	m_videoFrameBars = NULL;
	m_audioBuffer = NULL;
}

void CSignalGeneratorDlg::DoDataExchange(CDataExchange* pDX)
{
	CDialog::DoDataExchange(pDX);
	DDX_Control(pDX, IDOK, m_startButton);
	DDX_Control(pDX, IDC_COMBO_SIGNAL, m_outputSignalCombo);
	DDX_Control(pDX, IDC_COMBO_CHANNELS, m_audioChannelCombo);
	DDX_Control(pDX, IDC_COMBO_AUDIO_DEPTH, m_audioSampleDepthCombo);
	DDX_Control(pDX, IDC_COMBO_VIDEO_FORMAT, m_videoFormatCombo);
}

BEGIN_MESSAGE_MAP(CSignalGeneratorDlg, CDialog)
	ON_WM_SYSCOMMAND()
	ON_WM_PAINT()
	ON_WM_QUERYDRAGICON()
	//}}AFX_MSG_MAP
	ON_BN_CLICKED(IDOK, &CSignalGeneratorDlg::OnBnClickedOk)
END_MESSAGE_MAP()


// CSignalGeneratorDlg message handlers

BOOL CSignalGeneratorDlg::OnInitDialog()
{
	bool			success = false;
	
	CDialog::OnInitDialog();
	
	// Set the icon for this dialog.  The framework does this automatically
	//  when the application's main window is not a dialog
	SetIcon(m_hIcon, TRUE);			// Set big icon
	SetIcon(m_hIcon, FALSE);		// Set small icon
	
	
	// Initialize the DeckLink API
	IDeckLinkIterator*			deckLinkIterator = NULL;
	HRESULT						result;
	
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	result = CoCreateInstance(CLSID_CDeckLinkIterator, NULL, CLSCTX_ALL, IID_IDeckLinkIterator, (void**)&deckLinkIterator);
	if (FAILED(result))
	{
		MessageBox(_T("This application requires the DeckLink drivers installed.\nPlease install the Blackmagic DeckLink drivers to use the features of this application."), _T("Error"));
		goto bail;
	}
	
	// Connect to the first DeckLink instance
	result = deckLinkIterator->Next(&m_deckLink);
	if (result != S_OK)
	{
		MessageBox(_T("This application requires a DeckLink PCI card.\nYou will not be able to use the features of this application until a DeckLink PCI card is installed."), _T("Error"));
		goto bail;
	}
	
	// Obtain the audio/video output interface (IDeckLinkOutput)
	if (m_deckLink->QueryInterface(IID_IDeckLinkOutput, (void**)&m_deckLinkOutput) != S_OK)
		goto bail;
	
	// Provide this class as a delegate to the audio and video output interfaces
	m_deckLinkOutput->SetScheduledFrameCompletionCallback(this);
	m_deckLinkOutput->SetAudioCallback(this);
	
	
	// Populate the display mode combo with a list of display modes supported by the installed DeckLink card
	IDeckLinkDisplayModeIterator*		displayModeIterator;
	IDeckLinkDisplayMode*				deckLinkDisplayMode;
	
	m_videoFormatCombo.ResetContent();
	if (m_deckLinkOutput->GetDisplayModeIterator(&displayModeIterator) != S_OK)
		goto bail;
	while (displayModeIterator->Next(&deckLinkDisplayMode) == S_OK)
	{
		BSTR		modeName;
		
		if (deckLinkDisplayMode->GetName(&modeName) == S_OK)
		{
			CString		modeNameCString(modeName);
			int			newIndex;
			
			// Add this item to the video format poup menu
			newIndex = m_videoFormatCombo.AddString(modeNameCString);
			// Save the IDeckLinkDisplayMode as the ComboBox item's data
			if (newIndex >= 0)
				m_videoFormatCombo.SetItemDataPtr(newIndex, deckLinkDisplayMode);
			
			SysFreeString(modeName);
		}
	}
	displayModeIterator->Release();
	
	
	// Set the item data for combo box entries to store audio channel count and sample depth information
	m_outputSignalCombo.SetItemData(0, kOutputSignalPip);
	m_outputSignalCombo.SetItemData(1, kOutputSignalDrop);
	//
	m_audioChannelCombo.SetItemData(0, 2);		// 2 channels
	m_audioChannelCombo.SetItemData(1, 8);		// 8 channels
	m_audioChannelCombo.SetItemData(2, 16);		// 16 channels
	//
	m_audioSampleDepthCombo.SetItemData(0, 16);	// 16-bit samples
	m_audioSampleDepthCombo.SetItemData(1, 32);	// 32-bit samples
	
	// Select the first item in each combo box
	m_outputSignalCombo.SetCurSel(0);
	m_audioChannelCombo.SetCurSel(0);
	m_audioSampleDepthCombo.SetCurSel(0);
	m_videoFormatCombo.SetCurSel(0);
	
	success = true;
	
bail:
	if (success == false)
	{
		// Release any resources that were partially allocated
		if (m_deckLinkOutput != NULL)
		{
			m_deckLinkOutput->Release();
			m_deckLinkOutput = NULL;
		}
		//
		if (m_deckLink != NULL)
		{
			m_deckLink->Release();
			m_deckLink = NULL;
		}
		
		// Disable the user interface if we could not succsssfully connect to a DeckLink device
		m_startButton.EnableWindow(FALSE);
		EnableInterface(FALSE);
	}
	
	if (deckLinkIterator != NULL)
		deckLinkIterator->Release();
	
	return TRUE;  // return TRUE  unless you set the focus to a control
}

void CSignalGeneratorDlg::EnableInterface (BOOL enable)
{
	// Set the enable state of user interface elements
	m_outputSignalCombo.EnableWindow(enable);
	m_audioChannelCombo.EnableWindow(enable);
	m_audioSampleDepthCombo.EnableWindow(enable);
	m_videoFormatCombo.EnableWindow(enable);
}

void CSignalGeneratorDlg::OnBnClickedOk()
{
	if (m_running == false)
		StartRunning();
	else
		StopRunning();
}


void	CSignalGeneratorDlg::StartRunning ()
{
	IDeckLinkDisplayMode*	videoDisplayMode = NULL;
	
	// Determine the audio and video properties for the output stream
	m_outputSignal = (OutputSignal)m_outputSignalCombo.GetCurSel();
	m_audioChannelCount = m_audioChannelCombo.GetItemData(m_audioChannelCombo.GetCurSel());
	m_audioSampleDepth = (BMDAudioSampleType)m_audioSampleDepthCombo.GetItemData(m_audioSampleDepthCombo.GetCurSel());
	m_audioSampleRate = bmdAudioSampleRate48kHz;
	//
	// - Extract the IDeckLinkDisplayMode from the display mode popup menu (stashed in the item's tag)
	videoDisplayMode = (IDeckLinkDisplayMode*)m_videoFormatCombo.GetItemDataPtr(m_videoFormatCombo.GetCurSel());
	m_frameWidth = videoDisplayMode->GetWidth();
	m_frameHeight = videoDisplayMode->GetHeight();
	videoDisplayMode->GetFrameRate(&m_frameDuration, &m_frameTimescale);
	// Calculate the number of frames per second, rounded up to the nearest integer.  For example, for NTSC (29.97 FPS), framesPerSecond == 30.
	m_framesPerSecond = (unsigned long)((m_frameTimescale + (m_frameDuration-1))  /  m_frameDuration);
	
	// Set the video output mode
	if (m_deckLinkOutput->EnableVideoOutput(videoDisplayMode->GetDisplayMode(), bmdVideoOutputFlagDefault) != S_OK)
		goto bail;
	
	// Set the audio output mode
	if (m_deckLinkOutput->EnableAudioOutput(bmdAudioSampleRate48kHz, m_audioSampleDepth, m_audioChannelCount, bmdAudioOutputStreamTimestamped) != S_OK)
		goto bail;
	
	
	// Generate one second of audio tone
	m_audioSamplesPerFrame = (unsigned long)((m_audioSampleRate * m_frameDuration) / m_frameTimescale);
	m_audioBufferSampleLength = (unsigned long)((m_framesPerSecond * m_audioSampleRate * m_frameDuration) / m_frameTimescale);
	m_audioBuffer = HeapAlloc(GetProcessHeap(), 0, (m_audioBufferSampleLength * m_audioChannelCount * (m_audioSampleDepth / 8)));
	if (m_audioBuffer == NULL)
		goto bail;
	FillSine(m_audioBuffer, m_audioBufferSampleLength, m_audioChannelCount, m_audioSampleDepth);
	
	// Generate a frame of black
	if (m_deckLinkOutput->CreateVideoFrame(m_frameWidth, m_frameHeight, m_frameWidth*2, bmdFormat8BitYUV, bmdFrameFlagDefault, &m_videoFrameBlack) != S_OK)
		goto bail;
	FillBlack(m_videoFrameBlack);
	
	// Generate a frame of colour bars
	if (m_deckLinkOutput->CreateVideoFrame(m_frameWidth, m_frameHeight, m_frameWidth*2, bmdFormat8BitYUV, bmdFrameFlagDefault, &m_videoFrameBars) != S_OK)
		goto bail;
	FillColourBars(m_videoFrameBars);
	
	// Begin video preroll by scheduling a second of frames in hardware
	m_totalFramesScheduled = 0;
	for (unsigned i = 0; i < m_framesPerSecond; i++)
		ScheduleNextFrame(true);
	
	// Begin audio preroll.  This will begin calling our audio callback, which will start the DeckLink output stream.
	m_totalAudioSecondsScheduled = 0;
	if (m_deckLinkOutput->BeginAudioPreroll() != S_OK)
		goto bail;
	
	// Success; update the UI
	m_running = true;
	m_startButton.SetWindowText(_T("Stop"));
	// Disable the user interface while running (prevent the user from making changes to the output signal)
	EnableInterface(FALSE);
	
	return;
	
bail:
	// *** Error-handling code.  Cleanup any resources that were allocated. *** //
	StopRunning();
}


void	CSignalGeneratorDlg::StopRunning ()
{
	// Stop the audio and video output streams immediately
	m_deckLinkOutput->StopScheduledPlayback(0, NULL, 0);
	//
	m_deckLinkOutput->DisableAudioOutput();
	m_deckLinkOutput->DisableVideoOutput();
	
	if (m_videoFrameBlack != NULL)
		m_videoFrameBlack->Release();
	m_videoFrameBlack = NULL;
	
	if (m_videoFrameBars != NULL)
		m_videoFrameBars->Release();
	m_videoFrameBars = NULL;
	
	if (m_audioBuffer != NULL)
		HeapFree(GetProcessHeap(), 0, m_audioBuffer);
	m_audioBuffer = NULL;
	
	// Success; update the UI
	m_running = false;
	m_startButton.SetWindowText(_T("Start"));
	// Re-enable the user interface when stopped
	EnableInterface(TRUE);
}


void	CSignalGeneratorDlg::ScheduleNextFrame (bool prerolling)
{
	if (prerolling == false)
	{
		// If not prerolling, make sure that playback is still active
		if (m_running == false)
			return;
	}
	
	if (m_outputSignal == kOutputSignalPip)
	{
		if ((m_totalFramesScheduled % m_framesPerSecond) == 0)
		{
			// On each second, schedule a frame of bars
			if (m_deckLinkOutput->ScheduleVideoFrame(m_videoFrameBars, (m_totalFramesScheduled * m_frameDuration), m_frameDuration, m_frameTimescale) != S_OK)
				return;
		}
		else
		{
			// Schedue frames of black
			if (m_deckLinkOutput->ScheduleVideoFrame(m_videoFrameBlack, (m_totalFramesScheduled * m_frameDuration), m_frameDuration, m_frameTimescale) != S_OK)
				return;
		}
	}
	else
	{
		if ((m_totalFramesScheduled % m_framesPerSecond) == 0)
		{
			// On each second, schedule a frame of black
			if (m_deckLinkOutput->ScheduleVideoFrame(m_videoFrameBlack, (m_totalFramesScheduled * m_frameDuration), m_frameDuration, m_frameTimescale) != S_OK)
				return;
		}
		else
		{
			// Schedue frames of color bars
			if (m_deckLinkOutput->ScheduleVideoFrame(m_videoFrameBars, (m_totalFramesScheduled * m_frameDuration), m_frameDuration, m_frameTimescale) != S_OK)
				return;
		}
	}
	
	m_totalFramesScheduled += 1;
}

void	CSignalGeneratorDlg::WriteNextAudioSamples ()
{
	// Write one second of audio to the DeckLink API.
	
	if (m_outputSignal == kOutputSignalPip)
	{
		// Schedule one-frame of audio tone
		if (m_deckLinkOutput->ScheduleAudioSamples(m_audioBuffer, m_audioSamplesPerFrame, (m_totalAudioSecondsScheduled * m_audioBufferSampleLength), m_audioSampleRate, NULL) != S_OK)
			return;
	}
	else
	{
		// Schedule one-second (minus one frame) of audio tone
		if (m_deckLinkOutput->ScheduleAudioSamples(m_audioBuffer, (m_audioBufferSampleLength - m_audioSamplesPerFrame), (m_totalAudioSecondsScheduled * m_audioBufferSampleLength) + m_audioSamplesPerFrame, m_audioSampleRate, NULL) != S_OK)
			return;
	}
	
	m_totalAudioSecondsScheduled += 1;
}


/************************* DeckLink API Delegate Methods *****************************/

HRESULT		CSignalGeneratorDlg::ScheduledFrameCompleted (IDeckLinkVideoFrame* completedFrame, BMDOutputFrameCompletionResult result)
{
	// When a video frame has been 
	ScheduleNextFrame(false);
	return S_OK;
}

HRESULT		CSignalGeneratorDlg::ScheduledPlaybackHasStopped (void)
{
	return S_OK;
}

HRESULT		CSignalGeneratorDlg::RenderAudioSamples (BOOL preroll)
{
	// Provide further audio samples to the DeckLink API until our preferred buffer waterlevel is reached
	WriteNextAudioSamples();
	
	if (preroll)
	{
		// Start audio and video output
		m_deckLinkOutput->StartScheduledPlayback(0, 100, 1.0);
	}
	
	return S_OK;
}


// If you add a minimize button to your dialog, you will need the code below
//  to draw the icon.  For MFC applications using the document/view model,
//  this is automatically done for you by the framework.

void CSignalGeneratorDlg::OnPaint()
{
	if (IsIconic())
	{
		CPaintDC dc(this); // device context for painting

		SendMessage(WM_ICONERASEBKGND, reinterpret_cast<WPARAM>(dc.GetSafeHdc()), 0);

		// Center icon in client rectangle
		int cxIcon = GetSystemMetrics(SM_CXICON);
		int cyIcon = GetSystemMetrics(SM_CYICON);
		CRect rect;
		GetClientRect(&rect);
		int x = (rect.Width() - cxIcon + 1) / 2;
		int y = (rect.Height() - cyIcon + 1) / 2;

		// Draw the icon
		dc.DrawIcon(x, y, m_hIcon);
	}
	else
	{
		CDialog::OnPaint();
	}
}

// The system calls this function to obtain the cursor to display while the user drags
//  the minimized window.
HCURSOR CSignalGeneratorDlg::OnQueryDragIcon()
{
	return static_cast<HCURSOR>(m_hIcon);
}



/*****************************************/


void	FillSine (void* audioBuffer, unsigned long samplesToWrite, unsigned long channels, unsigned long sampleDepth)
{
	if (sampleDepth == 16)
	{
		INT16*		nextBuffer;
		
		nextBuffer = (INT16*)audioBuffer;
		for (unsigned i = 0; i < samplesToWrite; i++)
		{
			INT16		sample;
			
			sample = (INT16)(24576.0 * sin((i * 2.0 * M_PI) / 48.0));
			for (unsigned ch = 0; ch < channels; ch++)
				*(nextBuffer++) = sample;
		}
	}
	else if (sampleDepth == 32)
	{
		INT32*		nextBuffer;
		
		nextBuffer = (INT32*)audioBuffer;
		for (unsigned i = 0; i < samplesToWrite; i++)
		{
			INT32		sample;
			
			sample = (INT32)(1610612736.0 * sin((i * 2.0 * M_PI) / 48.0));
			for (unsigned ch = 0; ch < channels; ch++)
				*(nextBuffer++) = sample;
		}
	}
}

void	FillColourBars (IDeckLinkVideoFrame* theFrame)
{
	DWORD*			nextWord;
	unsigned long	width;
	unsigned long	height;
	DWORD*			bars;
	
	theFrame->GetBytes((void**)&nextWord);
	width = theFrame->GetWidth();
	height = theFrame->GetHeight();
	
	if (width > 720)
	{
		bars = gHD75pcColourBars;
	}
	else
	{
		bars = gSD75pcColourBars;
	}

	for (unsigned y = 0; y < height; y++)
	{
		for (unsigned x = 0; x < width; x+=2)
		{
			*(nextWord++) = bars[(x * 8) / width];
		}
	}
}

void	FillBlack (IDeckLinkVideoFrame* theFrame)
{
	DWORD*			nextWord;
	unsigned long	width;
	unsigned long	height;
	unsigned long	wordsRemaining;
	
	theFrame->GetBytes((void**)&nextWord);
	width = theFrame->GetWidth();
	height = theFrame->GetHeight();
	
	wordsRemaining = (width*2 * height) / 4;
	
	while (wordsRemaining-- > 0)
		*(nextWord++) = 0x10801080;
}