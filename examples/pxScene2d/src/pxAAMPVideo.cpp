/*

 pxCore Copyright 2005-2018 John Robinson

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

*/

// pxText.h

#include <iostream>
#include <string>
#include "pxAAMPVideo.h"

using namespace std::placeholders;

pxAAMPVideo *pxAAMPVideo::pxAAMPVideoObj = NULL;
GMainLoop *pxAAMPVideo::AAMPGstPlayerMainLoop;

extern pxContext context;
extern rtThreadQueue* gUIThreadQueue;

/**
 * @brief Thread to run mainloop (for standalone mode)
 * @param[in] arg user_data
 * @retval void pointer
 */
void* pxAAMPVideo::AAMPGstPlayer_StreamThread(void *arg)
{
  if (AAMPGstPlayerMainLoop)
  {
    g_main_loop_run(AAMPGstPlayerMainLoop); // blocks
    printf("AAMPGstPlayer_StreamThread: exited main event loop\n");
  }
  g_main_loop_unref(AAMPGstPlayerMainLoop);
  AAMPGstPlayerMainLoop = NULL;
  return NULL;
}

/**
 * @brief To initialize Gstreamer and start mainloop (for standalone mode)
 * @param[in] argc number of arguments
 * @param[in] argv array of arguments
 */
void pxAAMPVideo::InitPlayerLoop()
{
  if (!initialized)
  {
    initialized = true;
    gst_init(NULL, NULL);
    AAMPGstPlayerMainLoop = g_main_loop_new(NULL, FALSE);
    aampMainLoopThread = g_thread_new("AAMPGstPlayerLoop", &pxAAMPVideo::AAMPGstPlayer_StreamThread, NULL );
  }
}

void pxAAMPVideo::TermPlayerLoop()
{
	if(AAMPGstPlayerMainLoop)
	{
		g_main_loop_quit(AAMPGstPlayerMainLoop);
		g_thread_join(aampMainLoopThread);
		gst_deinit ();
		printf("%s(): Exited GStreamer MainLoop.\n", __FUNCTION__);
	}
}

pxAAMPVideo::pxAAMPVideo(pxScene2d* scene):pxObject(scene)
#ifdef ENABLE_SPARK_VIDEO_PUNCHTHROUGH
 ,mEnablePunchThrough(true)
#else
, mEnablePunchThrough(false)
#endif //ENABLE_SPARK_VIDEO_PUNCHTHROUGH
,mAutoPlay(true)
,mUrl("")
{
	  aampMainLoopThread = NULL;
	  AAMPGstPlayerMainLoop = NULL;
	  InitPlayerLoop();

	  std::function< void(uint8_t *, int, int, int) > cbExportFrames = nullptr;
	  if(!mEnablePunchThrough)
	  {
		  //Keeping this block to dynamically turn punch through on/off
		  //Spark will render frames
		  cbExportFrames = std::bind(&pxAAMPVideo::updateYUVFrame, this, _1, _2, _3, _4);
	  }
	  mAamp = new PlayerInstanceAAMP(NULL
#ifndef ENABLE_SPARK_VIDEO_PUNCHTHROUGH //TODO: Remove this check, once the official builds contain the second argument to PlayerInstanceAAMP
			  , cbExportFrames
#endif
			  );
	  assert (nullptr != mAamp);
	  pxAAMPVideo::pxAAMPVideoObj = this;
	  mYuvBuffer.buffer = NULL;
}

pxAAMPVideo::~pxAAMPVideo()
{
	mAamp->Stop();
	delete mAamp;
	TermPlayerLoop();
}

void pxAAMPVideo::onInit()
{
	rtLogError("%s:%d.",__FUNCTION__,__LINE__);
	if(mAutoPlay)
	{
		play();
	}
  mReady.send("resolve",this);
  pxObject::onInit();
}

void pxAAMPVideo::newAampFrame(void* context, void* data)
{
	pxAAMPVideo* videoObj = reinterpret_cast<pxAAMPVideo*>(context);
	if (videoObj)
	{
		videoObj->onTextureReady();
	}
}

inline unsigned char RGB_ADJUST(double tmp)
{
	return (unsigned char)((tmp >= 0 && tmp <= 255)?tmp:(tmp < 0 ? 0 : 255));
}

void CONVERT_YUV420PtoRGBA32(unsigned char* yuv420buf,unsigned char* rgbOutBuf,int nWidth,int nHeight)
{
	unsigned char Y,U,V,R,G,B;
	unsigned char* yPlane,*uPlane,*vPlane;
	int rgb_width , u_width;
	rgb_width = nWidth * 4;
	u_width = (nWidth >> 1);
	int offSet = 0;

	yPlane = yuv420buf;
	uPlane = yuv420buf + nWidth*nHeight;
	vPlane = uPlane + nWidth*nHeight/4;

	for(int i = 0; i < nHeight; i++)
	{
		for(int j = 0; j < nWidth; j ++)
		{
			Y = *(yPlane + nWidth * i + j);
			offSet = (i>>1) * (u_width) + (j>>1);
			V = *(uPlane + offSet);
			U = *(vPlane + offSet);

			//  R,G,B values
			R = RGB_ADJUST((Y + (1.4075 * (V - 128))));
			G = RGB_ADJUST((Y - (0.3455 * (U - 128) - 0.7169 * (V - 128))));
			B = RGB_ADJUST((Y + (1.7790 * (U - 128))));
			offSet = rgb_width * i + j * 4;

			rgbOutBuf[offSet] = B;
			rgbOutBuf[offSet + 1] = G;
			rgbOutBuf[offSet + 2] = R;
			rgbOutBuf[offSet + 3] = 255;
		}
	}
}

void pxAAMPVideo::updateYUVFrame(uint8_t *yuvBuffer, int size, int pixel_w, int pixel_h)
{
	/** Input in I420 (YUV420) format.
	  * Buffer structure:
	  * ----------
	  * |        |
	  * |   Y    | size = w*h
	  * |        |
	  * |________|
	  * |   U    |size = w*h/4
	  * |--------|
	  * |   V    |size = w*h/4
	  * ----------*
	  */
	if(yuvBuffer)
	{
		mYuvFrameMutex.lock();
		if (mYuvBuffer.buffer == NULL)
		{
			uint8_t *buffer = (uint8_t *) malloc(size);
			memcpy(buffer, yuvBuffer, size);
			mYuvBuffer.buffer = buffer;
			mYuvBuffer.size = size;
			mYuvBuffer.pixel_w = pixel_w;
			mYuvBuffer.pixel_h = pixel_h;
		}
		mYuvFrameMutex.unlock();

		gUIThreadQueue->addTask(newAampFrame, pxAAMPVideo::pxAAMPVideoObj, NULL);
	}
}

void pxAAMPVideo::draw()
{
  if (mEnablePunchThrough && !isRotated())
  {
    int screenX = 0;
    int screenY = 0;
    context.mapToScreenCoordinates(0,0,screenX, screenY);
    context.punchThrough(screenX,screenY,mw, mh);
  }
  else
  {
	YUVBUFFER yuvBuffer{NULL,0,0,0};
	mYuvFrameMutex.lock();
	if(mYuvBuffer.buffer)
	{
		yuvBuffer = mYuvBuffer;
		mYuvBuffer.buffer = NULL;
	}
	mYuvFrameMutex.unlock();

	if(yuvBuffer.buffer)
	{
		static pxTextureRef nullMaskRef;

		int rgbLen = yuvBuffer.pixel_w * yuvBuffer.pixel_h*4;
		uint8_t *buffer_convert = (uint8_t *) malloc(rgbLen);
		CONVERT_YUV420PtoRGBA32(yuvBuffer.buffer,buffer_convert,yuvBuffer.pixel_w, yuvBuffer.pixel_h);

		mOffscreen.init(yuvBuffer.pixel_w, yuvBuffer.pixel_h);
		mOffscreen.setBase(buffer_convert);
		pxTextureRef videoFrame = context.createTexture(mOffscreen);
		context.drawImage(0, 0, mw, mh,  videoFrame, nullMaskRef, false, NULL, pxConstantsStretch::STRETCH, pxConstantsStretch::STRETCH);
		free(yuvBuffer.buffer);
	}
  }
}

bool pxAAMPVideo::isRotated()
{
  pxMatrix4f matrix = context.getMatrix();
  float *f = matrix.data();
  const float e= 1.0e-2;

  if ( (fabsf(f[1]) > e) ||
       (fabsf(f[2]) > e) ||
       (fabsf(f[4]) > e) ||
       (fabsf(f[6]) > e) ||
       (fabsf(f[8]) > e) ||
       (fabsf(f[9]) > e) )
  {
    return true;
  }

  return false;
}

//properties
rtError pxAAMPVideo::availableAudioLanguages(rtObjectRef& /*v*/) const
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::availableClosedCaptionsLanguages(rtObjectRef& /*v*/) const
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::availableSpeeds(rtObjectRef& /*v*/) const
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::duration(float& /*v*/) const
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::zoom(rtString& /*v*/) const
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::setZoom(const char* /*s*/)
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::volume(uint32_t& /*v*/) const
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::setVolume(uint32_t /*v*/)
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::closedCaptionsOptions(rtObjectRef& /*v*/) const
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::setClosedCaptionsOptions(rtObjectRef /*v*/)
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::closedCaptionsLanguage(rtString& /*v*/) const
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::setClosedCaptionsLanguage(const char* /*s*/)
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::contentOptions(rtObjectRef& /*v*/) const
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::setContentOptions(rtObjectRef /*v*/)
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::speed(float& /*v*/) const
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::setSpeedProperty(float speed)
{
  if(mAamp)
  {
     mAamp->SetRate(speed);
  }
  return RT_OK;
}

rtError pxAAMPVideo::position(float& /*v*/) const
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::setPosition(float /*v*/)
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::audioLanguage(rtString& /*v*/) const
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::setAudioLanguage(const char* /*s*/)
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::secondaryAudioLanguage(rtString& /*v*/) const
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::setSecondaryAudioLanguage(const char* /*s*/)
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::url(rtString& url) const
{
	url = mUrl;
	return RT_OK;
}

rtError pxAAMPVideo::setUrl(const char* url)
{
	bool changingURL = false;
	if(!mUrl.isEmpty())
	{
		changingURL = true;
		stop();
	}

	mUrl = rtString(url);

	if(changingURL && mAutoPlay)
	{
		play();
	}
	rtLogError("%s:%d: URL[%s].",__FUNCTION__,__LINE__,url);
	return RT_OK;
}

rtError pxAAMPVideo::tsbEnabled(bool& /*v*/) const
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::setTsbEnabled(bool /*v*/)
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::closedCaptionsEnabled(bool& /*v*/) const
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::setClosedCaptionsEnabled(bool /*v*/)
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::autoPlay(bool& autoPlay) const
{
	autoPlay = mAutoPlay;
	return RT_OK;
}

rtError pxAAMPVideo::setAutoPlay(bool value)
{
	mAutoPlay = value;
	rtLogError("%s:%d: autoPlay[%s].",__FUNCTION__,__LINE__,value?"TRUE":"FALSE");
	return RT_OK;
}

rtError pxAAMPVideo::play()
{
	rtLogError("%s:%d.",__FUNCTION__,__LINE__);
	if(!mUrl.isEmpty())
	{
		mAamp->Tune(mUrl.cString());
	}
	return RT_OK;
}

rtError pxAAMPVideo::pause()
{
	if(mAamp)
	{
		mAamp->SetRate(0);
	}
	return RT_OK;
}

rtError pxAAMPVideo::stop()
{
	if(mAamp)
	{
		mAamp->Stop();
	}
	return RT_OK;
}

rtError pxAAMPVideo::setSpeed(float speed, float overshootCorrection)
{
	if(mAamp)
	{
		mAamp->SetRate(speed);
	}
	return RT_OK;
}

rtError pxAAMPVideo::setPositionRelative(float /*seconds*/)
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::requestStatus()
{
  //TODO
  return RT_OK;
}

rtError pxAAMPVideo::setAdditionalAuth(rtObjectRef /*params*/)
{
  //TODO
  return RT_OK;
}

rtDefineObject(pxAAMPVideo, pxObject);
rtDefineProperty(pxAAMPVideo, availableAudioLanguages);
rtDefineProperty(pxAAMPVideo, availableClosedCaptionsLanguages);
rtDefineProperty(pxAAMPVideo, availableSpeeds);
rtDefineProperty(pxAAMPVideo, duration);
rtDefineProperty(pxAAMPVideo, zoom);
rtDefineProperty(pxAAMPVideo, volume);
rtDefineProperty(pxAAMPVideo, closedCaptionsOptions);
rtDefineProperty(pxAAMPVideo, closedCaptionsLanguage);
rtDefineProperty(pxAAMPVideo, contentOptions);
rtDefineProperty(pxAAMPVideo, speed);
rtDefineProperty(pxAAMPVideo, position);
rtDefineProperty(pxAAMPVideo, audioLanguage);
rtDefineProperty(pxAAMPVideo, secondaryAudioLanguage);
rtDefineProperty(pxAAMPVideo, url);
rtDefineProperty(pxAAMPVideo, tsbEnabled);
rtDefineProperty(pxAAMPVideo, closedCaptionsEnabled);
rtDefineProperty(pxAAMPVideo, autoPlay);
rtDefineMethod(pxAAMPVideo, play);
rtDefineMethod(pxAAMPVideo, pause);
rtDefineMethod(pxAAMPVideo, stop);
rtDefineMethod(pxAAMPVideo, setSpeed);
rtDefineMethod(pxAAMPVideo, setPositionRelative);
rtDefineMethod(pxAAMPVideo, requestStatus);
rtDefineMethod(pxAAMPVideo, setAdditionalAuth);
