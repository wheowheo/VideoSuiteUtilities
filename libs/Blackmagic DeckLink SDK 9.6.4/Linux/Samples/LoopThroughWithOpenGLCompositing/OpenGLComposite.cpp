/* -LICENSE-START-
 ** Copyright (c) 2012 Blackmagic Design
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

#include "OpenGLComposite.h"
#include "GLExtensions.h"

OpenGLComposite::OpenGLComposite(QWidget *parent) :
	QGLWidget(parent), mParent(parent),
	mCaptureDelegate(NULL), mPlayoutDelegate(NULL),
	mDLInput(NULL), mDLOutput(NULL),
	mCaptureAllocator(NULL), mPlayoutAllocator(NULL),
	mFrameWidth(0), mFrameHeight(0),
	mHasNoInputSource(true),
	mPinnedMemoryExtensionAvailable(false),
	mTexture(0),
	mRotateAngle(0.0f),
	mRotateAngleRate(0.0f)
{
	ResolveGLExtensions(context());

	// Register non-builtin types for connecting signals and slots using these types
	qRegisterMetaType<IDeckLinkVideoInputFrame*>("IDeckLinkVideoInputFrame*");
	qRegisterMetaType<IDeckLinkVideoFrame*>("IDeckLinkVideoFrame*");
	qRegisterMetaType<BMDOutputFrameCompletionResult>("BMDOutputFrameCompletionResult");
}

OpenGLComposite::~OpenGLComposite()
{
	if (mDLInput != NULL)
	{
		// Cleanup for Capture
		mDLInput->SetCallback(NULL);

		mDLInput->Release();
		mDLInput = NULL;
	}

	delete mCaptureDelegate;
	delete mCaptureAllocator;

	if (mDLOutput != NULL)
	{
		// Cleanup for Playout
		mDLOutput->SetScheduledFrameCompletionCallback(NULL);

		mDLOutput->Release();
		mDLOutput = NULL;
	}

	delete mPlayoutDelegate;
	delete mPlayoutAllocator;
}

bool OpenGLComposite::InitDeckLink()
{
	bool							bSuccess = false;
	IDeckLinkIterator*				pDLIterator = NULL;
	IDeckLink*						pDL = NULL;
	IDeckLinkDisplayModeIterator*	pDLDisplayModeIterator = NULL;
	IDeckLinkDisplayMode*			pDLDisplayMode = NULL;
	BMDDisplayMode					displayMode = bmdModeNTSC;		// mode to use for capture and playout
	float							fps;

	pDLIterator = CreateDeckLinkIteratorInstance();
	if (pDLIterator == NULL)
	{
		QMessageBox::critical(NULL, "This application requires the DeckLink drivers installed.", "Please install the Blackmagic DeckLink drivers to use the features of this application.");
		return false;
	}

	while (pDLIterator->Next(&pDL) == S_OK)
	{
		// Use first board found as playout device, second board will be capture device
		if (! mDLOutput)
		{
			if (pDL->QueryInterface(IID_IDeckLinkOutput, (void**)&mDLOutput) != S_OK)
				goto error;
		}
		else if (! mDLInput)
		{
			if (pDL->QueryInterface(IID_IDeckLinkInput, (void**)&mDLInput) != S_OK)
				goto error;
		}
	}

	if (! mDLOutput || ! mDLInput)
	{
		QMessageBox::critical(NULL, "Expected both Input and Output DeckLink devices", "This application requires two DeckLink devices.");
		goto error;
	}

	if (mDLOutput->GetDisplayModeIterator(&pDLDisplayModeIterator) != S_OK)
	{
		QMessageBox::critical(NULL, "Cannot get Display Mode Iterator.", "DeckLink error.");
		goto error;
	}

	while (pDLDisplayModeIterator->Next(&pDLDisplayMode) == S_OK)
	{
		if (pDLDisplayMode->GetDisplayMode() == displayMode)
			break;

		pDLDisplayMode->Release();
		pDLDisplayMode = NULL;
	}
	pDLDisplayModeIterator->Release();

	if (pDLDisplayMode == NULL)
	{
		QMessageBox::critical(NULL, "Cannot get specified BMDDisplayMode.", "DeckLink error.");
		goto error;
	}

	mFrameWidth = pDLDisplayMode->GetWidth();
	mFrameHeight = pDLDisplayMode->GetHeight();

	// Compute a rotate angle rate so box will spin at a rate independent of video mode frame rate
	pDLDisplayMode->GetFrameRate(&mFrameDuration, &mFrameTimescale);
	fps = (float)mFrameTimescale / (float)mFrameDuration;
	mRotateAngleRate = 35.0f / fps;			// rotate box through 35 degrees every second

	// resize window to match video frame, but scale large formats down by half for viewing
	if (mFrameWidth < 1920)
		mParent->resize(mFrameWidth, mFrameHeight);
	else
		mParent->resize(mFrameWidth / 2, mFrameHeight / 2);

	// Check required extensions and setup OpenGL state
	if (! InitOpenGLState())
		goto error;

	// Capture will use a user-supplied frame memory allocator
	// For large frames use a reduced allocator frame cache size to avoid out-of-memory
	mCaptureAllocator = new PinnedMemoryAllocator(this, "Capture", mFrameWidth < 1920 ? 2 : 1);

	if (mDLInput->SetVideoInputFrameMemoryAllocator(mCaptureAllocator) != S_OK)
		goto error;

	if (mDLInput->EnableVideoInput(displayMode, bmdFormat8BitYUV, bmdVideoInputFlagDefault) != S_OK)
		goto error;

	mCaptureDelegate = new CaptureDelegate();
	if (mDLInput->SetCallback(mCaptureDelegate) != S_OK)
		goto error;

	// Playout will use a user-supplied frame memory allocator
	mPlayoutAllocator = new PinnedMemoryAllocator(this, "Playout", 1);

	if (mDLOutput->SetVideoOutputFrameMemoryAllocator(mPlayoutAllocator) != S_OK)
		goto error;

	if (mDLOutput->EnableVideoOutput(displayMode, bmdVideoOutputFlagDefault) != S_OK)
		goto error;

	// Create a queue of 35 IDeckLinkMutableVideoFrame objects to use for scheduling output video frames.
	// The ScheduledFrameCompleted() callback will immediately schedule a new frame using the next video frame from this queue.
	for (int i = 0; i < 35; i++)
	{
		// The frame read back from the GPU frame buffer and used for the playout video frame is in BGRA format.
		// The BGRA frame will be converted on playout to YCbCr either in hardware on most DeckLink cards or in software
		// within the DeckLink API for DeckLink devices without this hardware conversion.
		// If you want RGB 4:4:4 format to be played out "over the wire" in SDI, turn on the "Use 4:4:4 SDI" in the control
		// panel or turn on the bmdDeckLinkConfig444SDIVideoOutput flag using the IDeckLinkConfiguration interface.
		IDeckLinkMutableVideoFrame* outputFrame;
		if (mDLOutput->CreateVideoFrame(mFrameWidth, mFrameHeight, mFrameWidth*4, bmdFormat8BitBGRA, bmdFrameFlagFlipVertical, &outputFrame) != S_OK)
			goto error;

		mDLOutputVideoFrameQueue.push_back(outputFrame);
	}

	mPlayoutDelegate = new PlayoutDelegate();
	if (mPlayoutDelegate == NULL)
		goto error;

	if (mDLOutput->SetScheduledFrameCompletionCallback(mPlayoutDelegate) != S_OK)
		goto error;

	// Use signals and slots to ensure OpenGL rendering is performed on the main thread
	connect(mCaptureDelegate, SIGNAL(captureFrameArrived(IDeckLinkVideoInputFrame*, bool)), this, SLOT(VideoFrameArrived(IDeckLinkVideoInputFrame*, bool)), Qt::QueuedConnection);

	// When a scheduled playout frame completes, render and schedule the next frame to playout
	connect(mPlayoutDelegate, SIGNAL(playoutFrameCompleted(IDeckLinkVideoFrame*, BMDOutputFrameCompletionResult)), this, SLOT(PlayoutNextFrame(IDeckLinkVideoFrame*, BMDOutputFrameCompletionResult)), Qt::QueuedConnection);

	bSuccess = true;

error:
	if (!bSuccess)
	{
		if (mDLInput != NULL)
		{
			mDLInput->Release();
			mDLInput = NULL;
		}
		if (mDLOutput != NULL)
		{
			mDLOutput->Release();
			mDLOutput = NULL;
		}
	}

	if (pDL != NULL)
	{
		pDL->Release();
		pDL = NULL;
	}

	if (pDLIterator != NULL)
	{
		pDLIterator->Release();
		pDLIterator = NULL;
	}

	return bSuccess;
}

//
// QGLWidget virtual methods
//
void OpenGLComposite::initializeGL ()
{
	// Initialization is deferred to InitOpenGLState() when the width and height of the DeckLink video frame are known
}

void OpenGLComposite::paintGL ()
{
	// The DeckLink API provides IDeckLinkGLScreenPreviewHelper as a convenient way to view the playout video frames
	// in a window.  However, it performs a copy from host memory to the GPU which is wasteful in this case since
	// we already have the rendered frame to be played out sitting in the GPU in the mIdFrameBuf frame buffer.

	// Simply copy the off-screen frame buffer to on-screen frame buffer, scaling to the viewing window size.
	glBindFramebufferEXT(GL_READ_FRAMEBUFFER, mIdFrameBuf);
	glBindFramebufferEXT(GL_DRAW_FRAMEBUFFER, 0);
	glViewport(0, 0, mViewWidth, mViewHeight);
	glBlitFramebufferEXT(0, 0, mFrameWidth, mFrameHeight, 0, 0, mViewWidth, mViewHeight, GL_COLOR_BUFFER_BIT, GL_LINEAR);
}

void OpenGLComposite::resizeGL (int width, int height)
{
	// We don't set the project or model matrices here since the window data is copied directly from
	// an off-screen FBO in paintGL().  Just save the width and height for use in paintGL().
	mViewWidth = width;
	mViewHeight = height;
}

bool OpenGLComposite::InitOpenGLState()
{
	makeCurrent();

	if (! CheckOpenGLExtensions())
		return false;

	// Prepare the shader used to perform colour space conversion on the video texture
	char compilerErrorMessage[1024];
	if (! compileFragmentShader(sizeof(compilerErrorMessage), compilerErrorMessage))
	{
		QMessageBox::critical(NULL, compilerErrorMessage, "OpenGL Shader failed to compile");
		return false;
	}

	// Setup the scene
	glShadeModel( GL_SMOOTH );					// Enable smooth shading
	glClearColor( 0.0f, 0.0f, 0.0f, 0.5f );		// Black background
	glClearDepth( 1.0f );						// Depth buffer setup
	glEnable( GL_DEPTH_TEST );					// Enable depth testing
	glDepthFunc( GL_LEQUAL );					// Type of depth test to do
	glHint( GL_PERSPECTIVE_CORRECTION_HINT, GL_NICEST );

	if (! mPinnedMemoryExtensionAvailable)
	{
		glGenBuffers(1, &mUnpinnedTextureBuffer);
	}

	// Setup the texture which will hold the captured video frame pixels
	glEnable(GL_TEXTURE_2D);
	glGenTextures(1, &mTexture);
	glBindTexture(GL_TEXTURE_2D, mTexture);

	// Parameters to control how texels are sampled from the texture
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);

	// Create texture with empty data, we will update it using glTexSubImage2D each frame.
	// The captured video is YCbCr 4:2:2 packed into a UYVY macropixel.  OpenGL has no YCbCr format
	// so treat it as RGBA 4:4:4:4 by halving the width and using GL_RGBA internal format.
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, mFrameWidth/2, mFrameHeight, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);

	glBindTexture(GL_TEXTURE_2D, 0);
	glDisable(GL_TEXTURE_2D);

	// Create Frame Buffer Object (FBO) to perform off-screen rendering of scene.
	// This allows the render to be done on a framebuffer with width and height exactly matching the video format.
	glGenFramebuffersEXT(1, &mIdFrameBuf);
	glGenRenderbuffersEXT(1, &mIdColorBuf);
	glGenRenderbuffersEXT(1, &mIdDepthBuf);

	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, mIdFrameBuf);

	glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, mIdColorBuf);
	glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, mFrameWidth, mFrameHeight);
	glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, mIdDepthBuf);
	glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_COMPONENT, mFrameWidth, mFrameHeight);

	glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, mIdColorBuf);
	glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, mIdDepthBuf);

	GLenum glStatus = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
	if (glStatus != GL_FRAMEBUFFER_COMPLETE_EXT)
	{
		QMessageBox::critical(NULL, "Cannot initialize framebuffer.", "OpenGL initialization error.");
		return false;
	}

	return true;
}

//
// Update the captured video frame texture
//
void OpenGLComposite::VideoFrameArrived(IDeckLinkVideoInputFrame* inputFrame, bool hasNoInputSource)
{
	mMutex.lock();

	mHasNoInputSource = hasNoInputSource;

	long textureSize = inputFrame->GetRowBytes() * inputFrame->GetHeight();
	void* videoPixels;
	inputFrame->GetBytes(&videoPixels);

	makeCurrent();

	glEnable(GL_TEXTURE_2D);

	if (! mPinnedMemoryExtensionAvailable)
	{
		// Use a normal texture buffer
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, mUnpinnedTextureBuffer);
		glBufferData(GL_PIXEL_UNPACK_BUFFER, textureSize, videoPixels, GL_DYNAMIC_DRAW);
	}
	else
	{
		// Use a pinned buffer for the GL_PIXEL_UNPACK_BUFFER target
		glBindBuffer(GL_PIXEL_UNPACK_BUFFER, mCaptureAllocator->bufferObjectForPinnedAddress(textureSize, videoPixels));
	}
	glBindTexture(GL_TEXTURE_2D, mTexture);

	// NULL for last arg indicates use current GL_PIXEL_UNPACK_BUFFER target as texture data
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, mFrameWidth/2, mFrameHeight, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);

	if (mPinnedMemoryExtensionAvailable)
	{
		// Ensure pinned texture has been transferred to GPU before we draw with it
		GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 40 * 1000 * 1000);	// timeout in nanosec
		glDeleteSync(fence);
	}

	glBindTexture(GL_TEXTURE_2D, 0);
	glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	glDisable(GL_TEXTURE_2D);

	mMutex.unlock();
	inputFrame->Release();
}

// Draw the captured video frame texture onto a box, rendering to the off-screen frame buffer.
// Read the rendered scene back from the frame buffer and schedule it for playout.
void OpenGLComposite::PlayoutNextFrame(IDeckLinkVideoFrame* completedFrame, BMDOutputFrameCompletionResult completionResult)
{
	mMutex.lock();

	// Use the frame from the front of the queue and add the completed frame to the back of the queue
	mDLOutputVideoFrameQueue.push_back( dynamic_cast<IDeckLinkMutableVideoFrame*>(completedFrame) );
	IDeckLinkMutableVideoFrame* outputVideoFrame = mDLOutputVideoFrameQueue.front();
	mDLOutputVideoFrameQueue.pop_front();

	void*	pFrame;
	outputVideoFrame->GetBytes(&pFrame);

	long rowbytes = outputVideoFrame->GetRowBytes();
	long height = outputVideoFrame->GetHeight();
	long memSize = rowbytes * height;

	// make GL context current
	makeCurrent();

	// Draw OpenGL scene to the off-screen frame buffer
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, mIdFrameBuf);

	// Setup view and projection
	GLfloat aspectRatio = (GLfloat)mFrameWidth / (GLfloat)mFrameHeight;
	glViewport (0, 0, mFrameWidth, mFrameHeight);
	glMatrixMode( GL_PROJECTION );
	glLoadIdentity();
	gluPerspective( 45.0f, aspectRatio, 0.1f, 100.0f );
	glMatrixMode( GL_MODELVIEW );
	glLoadIdentity();

	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	glScalef( aspectRatio, 1.0f, 1.0f );			// Scale x for correct aspect ratio
	glTranslatef( 0.0f, 0.0f, -4.0f );				// Move into screen
	glRotatef( mRotateAngle, 1.0f, 1.0f, 1.0f );	// Rotate model around a vector
	mRotateAngle -= mRotateAngleRate;				// update the rotation angle for next iteration
	glFinish();										// Ensure changes to GL state are complete

	// Draw a colourful frame around the front face of the box
	// (provides a pleasing nesting effect when you connect the playout output to the capture input)
	glBegin(GL_QUAD_STRIP);
	glColor3f( 1.0f, 0.0f, 0.0f );
	glVertex3f( 1.2f,  1.2f, 1.0f);
	glVertex3f( 1.0f,  1.0f, 1.0f);
	glColor3f( 0.0f, 0.0f, 1.0f );
	glVertex3f( 1.2f, -1.2f, 1.0f);
	glVertex3f( 1.0f, -1.0f, 1.0f);
	glColor3f( 0.0f, 1.0f, 0.0f );
	glVertex3f(-1.2f, -1.2f, 1.0f);
	glVertex3f(-1.0f, -1.0f, 1.0f);
	glColor3f( 1.0f, 1.0f, 0.0f );
	glVertex3f(-1.2f,  1.2f, 1.0f);
	glVertex3f(-1.0f,  1.0f, 1.0f);
	glColor3f( 1.0f, 0.0f, 0.0f );
	glVertex3f( 1.2f,  1.2f, 1.0f);
	glVertex3f( 1.0f,  1.0f, 1.0f);
	glEnd();

	if (mHasNoInputSource)
	{
		// Draw a big X when no input is available on capture
		glBegin( GL_QUADS );
		glColor3f( 1.0f, 0.0f, 1.0f );
		glVertex3f(  0.8f,  0.9f,  1.0f );
		glVertex3f(  0.9f,  0.8f,  1.0f );
		glColor3f( 1.0f, 1.0f, 0.0f );
		glVertex3f( -0.8f, -0.9f,  1.0f );
		glVertex3f( -0.9f, -0.8f,  1.0f );
		glColor3f( 1.0f, 0.0f, 1.0f );
		glVertex3f( -0.8f,  0.9f,  1.0f );
		glVertex3f( -0.9f,  0.8f,  1.0f );
		glColor3f( 1.0f, 1.0f, 0.0f );
		glVertex3f(  0.8f, -0.9f,  1.0f );
		glVertex3f(  0.9f, -0.8f,  1.0f );
		glEnd();
	}
	else
	{
		// Pass texture unit 0 to the fragment shader as a uniform variable
		glEnable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, mTexture);
		glUseProgram(mProgram);
		GLint locUYVYtex = glGetUniformLocation(mProgram, "UYVYtex");
		glUniform1i(locUYVYtex, 0);		// Bind texture unit 0

		// Draw front and back faces of box applying video texture to each face
		glBegin(GL_QUADS);
		glTexCoord2f(1.0f, 0.0f);	glVertex3f(  1.0f,  1.0f,  1.0f );		// Top right of front side
		glTexCoord2f(0.0f, 0.0f);	glVertex3f( -1.0f,  1.0f,  1.0f );		// Top left of front side
		glTexCoord2f(0.0f, 1.0f);	glVertex3f( -1.0f, -1.0f,  1.0f );		// Bottom left of front side
		glTexCoord2f(1.0f, 1.0f);	glVertex3f(  1.0f, -1.0f,  1.0f );		// Bottom right of front side

		glTexCoord2f(1.0f, 1.0f);	glVertex3f(  1.0f, -1.0f, -1.0f );		// Top right of back side
		glTexCoord2f(0.0f, 1.0f);	glVertex3f( -1.0f, -1.0f, -1.0f );		// Top left of back side
		glTexCoord2f(0.0f, 0.0f);	glVertex3f( -1.0f,  1.0f, -1.0f );		// Bottom left of back side
		glTexCoord2f(1.0f, 0.0f);	glVertex3f(  1.0f,  1.0f, -1.0f );		// Bottom right of back side
		glEnd();

		// Draw left and right sides of box with partially transparent video texture
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glBegin(GL_QUADS);
		glTexCoord2f(0.1f, 0.0f);	glVertex3f( -1.0f,  1.0f,  1.0f );		// Top right of left side
		glTexCoord2f(1.0f, 0.0f);	glVertex3f( -1.0f,  1.0f, -1.0f );		// Top left of left side
		glTexCoord2f(1.0f, 1.0f);	glVertex3f( -1.0f, -1.0f, -1.0f );		// Bottom left of left side
		glTexCoord2f(0.1f, 1.0f);	glVertex3f( -1.0f, -1.0f,  1.0f );		// Bottom right of left side

		glTexCoord2f(1.0f, 0.0f);	glVertex3f(  1.0f,  1.0f, -1.0f );		// Top right of right side
		glTexCoord2f(0.0f, 0.0f);	glVertex3f(  1.0f,  1.0f,  1.0f );		// Top left of right side
		glTexCoord2f(0.0f, 1.0f);	glVertex3f(  1.0f, -1.0f,  1.0f );		// Bottom left of right side
		glTexCoord2f(1.0f, 1.0f);	glVertex3f(  1.0f, -1.0f, -1.0f );		// Bottom right of right side
		glEnd();
		glDisable(GL_BLEND);

		glUseProgram(0);
		glDisable(GL_TEXTURE_2D);
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	if (! mPinnedMemoryExtensionAvailable)
	{
		glReadPixels(0, 0, mFrameWidth, mFrameHeight, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, pFrame);
	}
	else
	{
		// Use a PIXEL PACK BUFFER to read back pixels
		glBindBuffer(GL_PIXEL_PACK_BUFFER, mPlayoutAllocator->bufferObjectForPinnedAddress(memSize, pFrame));
		glReadPixels(0, 0, mFrameWidth, mFrameHeight, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);

		// Ensure GPU has processed all commands in the pipeline up to this point, before pFrame is read by the CPU
		GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 40 * 1000 * 1000);	// timeout in nanosec
		glDeleteSync(fence);
	}

	// If the last completed frame was late or dropped, bump the scheduled time further into the future by one frame
	if (completionResult == bmdOutputFrameDisplayedLate || completionResult == bmdOutputFrameDropped)
		mTotalPlayoutFrames += 2;

	// Schedule the next frame for playout
	HRESULT hr = mDLOutput->ScheduleVideoFrame(outputVideoFrame, (mTotalPlayoutFrames * mFrameDuration), mFrameDuration, mFrameTimescale);
	if (SUCCEEDED(hr))
		mTotalPlayoutFrames++;

	mMutex.unlock();

	updateGL();				// Trigger the QGLWidget to repaint the on-screen window in paintGL()
}

bool OpenGLComposite::Start()
{
	mTotalPlayoutFrames = 0;

	// Preroll frames
	for (unsigned i = 0; i < mDLOutputVideoFrameQueue.size(); i++)
	{
		// Take each video frame from the front of the queue and move it to the back
		IDeckLinkMutableVideoFrame* outputVideoFrame = mDLOutputVideoFrameQueue.front();
		mDLOutputVideoFrameQueue.push_back(outputVideoFrame);
		mDLOutputVideoFrameQueue.pop_front();

		// Start with a black frame for playout
		void*	pFrame;
		outputVideoFrame->GetBytes((void**)&pFrame);
		memset(pFrame, 0, outputVideoFrame->GetRowBytes() * mFrameHeight);		// 0 is black in RGBA format

		if (mDLOutput->ScheduleVideoFrame(outputVideoFrame, (mTotalPlayoutFrames * mFrameDuration), mFrameDuration, mFrameTimescale) != S_OK)
			return false;

		mTotalPlayoutFrames++;
	}

	mDLInput->StartStreams();
	mDLOutput->StartScheduledPlayback(0, 100, 1.0);

	return true;
}

bool OpenGLComposite::Stop()
{
	mDLInput->StopStreams();
	mDLInput->DisableVideoInput();

	mDLOutput->StopScheduledPlayback(0, NULL, 0);
	mDLOutput->DisableVideoOutput();

	return true;
}

// Setup fragment shader to take YCbCr 4:2:2 video texture in UYVY macropixel format
// and perform colour space conversion to RGBA in the GPU.
bool OpenGLComposite::compileFragmentShader(int errorMessageSize, char* errorMessage)
{
	GLsizei		errorBufferSize;
	GLint		compileResult, linkResult;
	const char*	fragmentSource =
		"#version 130 \n"
		"uniform sampler2D UYVYtex; \n"		// UYVY macropixel texture passed as RGBA format
		"void main(void) \n"
		"{\n"
		"	float tx, ty, Y, Cb, Cr, r, g, b; \n"
		"	tx = gl_TexCoord[0].x; \n"
		"	ty = gl_TexCoord[0].y; \n"

			// The UYVY texture appears to the shader with 1/2 the true width since we used RGBA format to pass UYVY
		"	int true_width = textureSize(UYVYtex, 0).x * 2; \n"

			// For U0 Y0 V0 Y1 macropixel, lookup Y0 or Y1 based on whether
			// the original texture x coord is even or odd.
		"	if (fract(floor(tx * true_width + 0.5) / 2.0) > 0.0) \n"
		"		Y = texture2D(UYVYtex, vec2(tx,ty)).a; \n"		// odd so choose Y1
		"	else \n"
		"		Y = texture2D(UYVYtex, vec2(tx,ty)).g; \n"		// even so choose Y0
		"	Cb = texture2D(UYVYtex, vec2(tx,ty)).b; \n"
		"	Cr = texture2D(UYVYtex, vec2(tx,ty)).r; \n"

			// Y: Undo 1/256 texture value scaling and scale [16..235] to [0..1] range
			// C: Undo 1/256 texture value scaling and scale [16..240] to [-0.5 .. + 0.5] range
		"	Y = (Y * 256.0 - 16.0) / 219.0; \n"
		"	Cb = (Cb * 256.0 - 16.0) / 224.0 - 0.5; \n"
		"	Cr = (Cr * 256.0 - 16.0) / 224.0 - 0.5; \n"
			// Convert to RGB using Rec.709 conversion matrix (see eq 26.7 in Poynton 2003)
		"	r = Y + 1.5748 * Cr; \n"
		"	g = Y - 0.1873 * Cb - 0.4681 * Cr; \n"
		"	b = Y + 1.8556 * Cb; \n"

			// Set alpha to 0.7 for partial transparency when GL_BLEND is enabled
		"	gl_FragColor = vec4(r, g, b, 0.7); \n"
		"}\n";

	mFragmentShader = glCreateShader(GL_FRAGMENT_SHADER);

	glShaderSource(mFragmentShader, 1, (const GLchar**)&fragmentSource, NULL);
	glCompileShader(mFragmentShader);

	glGetShaderiv(mFragmentShader, GL_COMPILE_STATUS, &compileResult);
	if (compileResult == GL_FALSE)
	{
		glGetShaderInfoLog(mFragmentShader, errorMessageSize, &errorBufferSize, errorMessage);
		return false;
	}

	mProgram = glCreateProgram();

	glAttachShader(mProgram, mFragmentShader);
	glLinkProgram(mProgram);

	glGetProgramiv(mProgram, GL_LINK_STATUS, &linkResult);
	if (linkResult == GL_FALSE)
	{
		glGetProgramInfoLog(mProgram, errorMessageSize, &errorBufferSize, errorMessage);
		return false;
	}

	return true;
}

bool OpenGLComposite::CheckOpenGLExtensions()
{
	const GLubyte* strExt;
	GLboolean hasFBO, hasPinned;

	if (! isValid())
	{
		QMessageBox::critical(NULL,"OpenGL initialization error.", "OpenGL context is not valid for specified QGLFormat.");
		return false;
	}

	makeCurrent();
	strExt = glGetString (GL_EXTENSIONS);
	hasFBO = gluCheckExtension ((const GLubyte*)"GL_EXT_framebuffer_object", strExt);
	hasPinned = gluCheckExtension ((const GLubyte*)"GL_AMD_pinned_memory", strExt);

	mPinnedMemoryExtensionAvailable = hasPinned;

	if (!hasFBO)
	{
		QMessageBox::critical(NULL,"OpenGL initialization error.", "OpenGL extension \"GL_EXT_framebuffer_object\" is not supported.");
		return false;
	}

	if (!mPinnedMemoryExtensionAvailable)
		fprintf(stderr, "GL_AMD_pinned_memory extension not available, using regular texture buffer fallback instead\n");

	return true;
}

////////////////////////////////////////////
// PinnedMemoryAllocator
////////////////////////////////////////////

// PinnedMemoryAllocator implements the IDeckLinkMemoryAllocator interface and can be used instead of the
// built-in frame allocator, by setting with SetVideoInputFrameMemoryAllocator() or SetVideoOutputFrameMemoryAllocator().
//
// For this sample application a custom frame memory allocator is used to ensure each address
// of frame memory is aligned on a 4kB boundary required by the OpenGL pinned memory extension.
// If the pinned memory extension is not available, this allocator will still be used and
// demonstrates how to cache frame allocations for efficiency.
//
// The frame cache delays the releasing of buffers until the cache fills up, thereby avoiding an
// allocate plus pin operation for every frame, followed by an unpin and deallocate on every frame.

PinnedMemoryAllocator::PinnedMemoryAllocator(QGLWidget* context, const char *name, unsigned cacheSize) :
	mContext(context),
	mRefCount(1),
	mName(name),
	mFrameCacheSize(cacheSize)	// large cache size will keep more GPU memory pinned and may result in out of memory errors
{
}

PinnedMemoryAllocator::~PinnedMemoryAllocator()
{
}

GLuint PinnedMemoryAllocator::bufferObjectForPinnedAddress(int bufferSize, const void* address)
{
	// Store all input memory buffers in a map to lookup corresponding pinned buffer handle
	if (mBufferHandleForPinnedAddress.count(address) == 0)
	{
		// This method assumes the OpenGL context is current

		// Create a handle to use for pinned memory
		GLuint bufferHandle;
		glGenBuffers(1, &bufferHandle);

		// Pin memory by binding buffer to special AMD target.
		glBindBuffer(GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD, bufferHandle);

		// glBufferData() sets up the address so any OpenGL operation on this buffer will use client memory directly
		// (assumes address is aligned to 4k boundary).
		glBufferData(GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD, bufferSize, address, GL_STREAM_DRAW);
		GLenum result = glGetError();
		if (result != GL_NO_ERROR)
		{
			fprintf(stderr, "%s allocator: Error pinning memory with glBufferData(GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD, size=%d ...) error=%s\n", mName, bufferSize, gluErrorString(result));
			exit(1);
		}
		glBindBuffer(GL_EXTERNAL_VIRTUAL_MEMORY_BUFFER_AMD, 0);		// Unbind buffer to target

		mBufferHandleForPinnedAddress[address] = bufferHandle;
	}

	return mBufferHandleForPinnedAddress[address];
}

void PinnedMemoryAllocator::unPinAddress(const void* address)
{
	// un-pin address only if it has been pinned
	if (mBufferHandleForPinnedAddress.count(address) > 0)
	{
		mContext->makeCurrent();

		// The buffer is un-pinned by the GPU when the buffer is deleted
		GLuint bufferHandle = mBufferHandleForPinnedAddress[address];
		glDeleteBuffers(1, &bufferHandle);
		mBufferHandleForPinnedAddress.erase(address);
	}
}

// IUnknown methods
HRESULT STDMETHODCALLTYPE	PinnedMemoryAllocator::QueryInterface(REFIID /*iid*/, LPVOID* /*ppv*/)
{
	return E_NOTIMPL;
}

ULONG STDMETHODCALLTYPE		PinnedMemoryAllocator::AddRef(void)
{
	int oldValue = mRefCount.fetchAndAddAcquire(1);
	return (ULONG)(oldValue + 1);
}

ULONG STDMETHODCALLTYPE		PinnedMemoryAllocator::Release(void)
{
	int oldValue = mRefCount.fetchAndAddAcquire(-1);
	if (oldValue == 1)		// i.e. current value will be 0
		delete this;

	return (ULONG)(oldValue - 1);
}

// IDeckLinkMemoryAllocator methods
HRESULT STDMETHODCALLTYPE	PinnedMemoryAllocator::AllocateBuffer (uint32_t bufferSize, void* *allocatedBuffer)
{
	if (mFrameCache.empty())
	{
		// alignment to 4K required when pinning memory
		if (posix_memalign(allocatedBuffer, 4096, bufferSize) != 0)
			return E_OUTOFMEMORY;
	}
	else
	{
		// Re-use most recently ReleaseBuffer'd address
		*allocatedBuffer = mFrameCache.back();
		mFrameCache.pop_back();
	}
	return S_OK;
}

HRESULT STDMETHODCALLTYPE	PinnedMemoryAllocator::ReleaseBuffer (void* buffer)
{
	if (mFrameCache.size() < mFrameCacheSize)
	{
		mFrameCache.push_back(buffer);
	}
	else
	{
		// No room left in cache, so un-pin (if it was pinned) and free this buffer
		unPinAddress(buffer);
		free(buffer);
	}
	return S_OK;
}

HRESULT STDMETHODCALLTYPE	PinnedMemoryAllocator::Commit ()
{
	return S_OK;
}

HRESULT STDMETHODCALLTYPE	PinnedMemoryAllocator::Decommit ()
{
	while (! mFrameCache.empty())
	{
		// Cleanup any frames allocated and pinned in AllocateBuffer() but not freed in ReleaseBuffer()
		unPinAddress( mFrameCache.back() );
		free( mFrameCache.back() );
		mFrameCache.pop_back();
	}
	return S_OK;
}

////////////////////////////////////////////
// DeckLink Capture Delegate Class
////////////////////////////////////////////
HRESULT	CaptureDelegate::VideoInputFrameArrived(IDeckLinkVideoInputFrame* inputFrame, IDeckLinkAudioInputPacket* /*audioPacket*/)
{
	if (! inputFrame)
	{
		// It's possible to receive a NULL inputFrame, but a valid audioPacket. Ignore audio-only frame.
		return S_OK;
	}

	bool hasNoInputSource = inputFrame->GetFlags() & bmdFrameHasNoInputSource;

	// emit just adds a message to Qt's event queue since we're in a different thread, so add a reference
	// to the input frame to prevent it getting released before the connected slot can process the frame.
	inputFrame->AddRef();
	emit captureFrameArrived(inputFrame, hasNoInputSource);
	return S_OK;
}

HRESULT	CaptureDelegate::VideoInputFormatChanged(BMDVideoInputFormatChangedEvents /*notificationEvents*/, IDeckLinkDisplayMode* /*newDisplayMode*/, BMDDetectedVideoInputFormatFlags /*detectedSignalFlags*/)
{
	fprintf(stderr, "VideoInputFormatChanged()\n");
	return S_OK;
}

////////////////////////////////////////////
// DeckLink Playout Delegate Class
////////////////////////////////////////////
HRESULT	PlayoutDelegate::ScheduledFrameCompleted (IDeckLinkVideoFrame* completedFrame, BMDOutputFrameCompletionResult result)
{
	// Don't log bmdOutputFrameFlushed result since it is expected when Stop() is called
	if (result != bmdOutputFrameCompleted && result != bmdOutputFrameFlushed)
	{
		const char* message = "Unknown error";
		switch (result)
		{
			case bmdOutputFrameDisplayedLate:
				message = "Frame Displayed Late";
				break;
			case bmdOutputFrameDropped:
				message = "Frame Dropped";
				break;
		}
		fprintf(stderr, "ScheduledFrameCompleted() frame did not complete: %s\n", message);
	}

	emit playoutFrameCompleted(completedFrame, result);
	return S_OK;
}

HRESULT	PlayoutDelegate::ScheduledPlaybackHasStopped ()
{
	return S_OK;
}
