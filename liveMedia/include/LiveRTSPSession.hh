#ifndef _LIVE_RTSP_SESSION
#define _LIVE_RTSP_SESSION

#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"
#include <pthread.h>

class RTPRelaySink;

class LiveRTSPSession {
public:
	static LiveRTSPSession* createNew(char const* rtspURL, char const* progName = NULL);
	virtual ~LiveRTSPSession();

	typedef void (OnGettingSDPFunc)(void* callbackData, char* sdpInfo);
	typedef void (OnGettingPacketFunc)(void* callbackData, char* packetData,
					unsigned packetSize, int trackIndex);

	int Start(OnGettingSDPFunc* onGettingSDPFunc,
				OnGettingPacketFunc* onGettingPacketFunc,
				void* callbackData);
	void StopAndWait();

	char const* GetSDPInfo() { return fSDPInfo; }
	char const* GetURL() { return fURL; }

private:
	LiveRTSPSession(char const* rtspURL, char const* progName = NULL);
	// called only by createNew

	void NotifySDPInfo(char* const sdpInfo);
	void NotifyPacket(char* packetData, unsigned packetSize, int trackIndex);

	static void* thread_entry(void* session);

	static void continueAfterDESCRIBE(RTSPClient* rtspClient, int resultCode, char* resultString);
	static void continueAfterSETUP(RTSPClient* rtspClient, int resultCode, char* resultString);
	static void continueAfterPLAY(RTSPClient* rtspClient, int resultCode, char* resultString);

	// Other event handler functions:
	static void subsessionAfterPlaying(void* clientData); // called when a stream's subsession (e.g., audio or video substream) ends
	static void subsessionByeHandler(void* clientData); // called when a RTCP "BYE" is received for a subsession
	static void streamTimerHandler(void* clientData);
		// called at the end of a stream's expected duration (if the stream has not already signaled its end using a RTCP "BYE")

	// Used to iterate through each stream's 'subsessions', setting up each one:
	static void setupNextSubsession(RTSPClient* rtspClient);

	// Used to shut down and close a stream (including its "RTSPClient" object):
	static void shutdownStream(RTSPClient* rtspClient, int exitCode = 1);

	const static u_int16_t kMaxURLLen = 64;
	const static u_int16_t kMaxSDPLen = 1024;

	RTSPClient*          fRTSPClient;
	char                 fURL[kMaxURLLen];
	char                 fSDPInfo[kMaxSDPLen];
	TaskScheduler*       fScheduler;
	UsageEnvironment*    fEnv;

	OnGettingSDPFunc*    fOnGettingSDPFunc;
	OnGettingPacketFunc* fOnGettingPacketFunc;
	void*                fCallbackData;

	char                 fThreadWatchVariable; // keep this as 0 if you do not want to break the event loop.
	bool                 fThreadStarted;
	pthread_t            fThread;

	friend class RTPRelaySink;
};

// Define a class to hold per-stream state that we maintain throughout each stream's lifetime:

class StreamClientState {
public:
	StreamClientState();
	virtual ~StreamClientState();

public:
	MediaSubsessionIterator* iter;
	MediaSession*            session;
	MediaSubsession*         subsession;
	TaskToken                streamTimerTask;
	double                   duration;
};


// If you're streaming just a single stream (i.e., just from a single URL, once), then you can define and use just a single
// "StreamClientState" structure, as a global variable in your application.  However, because - in this demo application - we're
// showing how to play multiple streams, concurrently, we can't do that.  Instead, we have to have a separate "StreamClientState"
// structure for each "RTSPClient".  To do this, we subclass "RTSPClient", and add a "StreamClientState" field to the subclass:

class LiveRTSPClient: public RTSPClient {
public:
  static LiveRTSPClient* createNew(UsageEnvironment& env, char const* rtspURL,
					LiveRTSPSession& liveRTSPSession,
					int verbosityLevel = 0,
					char const* applicationName = NULL);

  LiveRTSPSession& liveRTSPSession() { return fLiveRTSPSession; }

protected:
	LiveRTSPClient(UsageEnvironment& env, char const* rtspURL,
		LiveRTSPSession& liveRTSPSession,
		int verbosityLevel, char const* applicationName);
	// called only by createNew();
	virtual ~LiveRTSPClient();

public:
	StreamClientState scs;

private:
	LiveRTSPSession&  fLiveRTSPSession;
};

// Define a data sink (a subclass of "MediaSink") to receive the data for each subsession (i.e., each audio or video 'substream').
// In practice, this might be a class (or a chain of classes) that decodes and then renders the incoming audio or video.
// Or it might be a "FileSink", for outputting the received data into a file (as is done by the "openRTSP" application).
// In this example code, however, we define a simple 'dummy' sink that receives incoming data, but does nothing with it.
class RTPRelaySink: public MediaSink {
public:
	static RTPRelaySink* createNew(UsageEnvironment& env,
						MediaSubsession& subsession, // identifies the kind of data that's being received
						LiveRTSPSession& liveRTSPSession,
						char const* streamId = NULL); // identifies the stream itself (optional)

private:
	RTPRelaySink(UsageEnvironment& env, MediaSubsession& subsession, LiveRTSPSession& liveRTSPSession, char const* streamId);
	// called only by "createNew()"
	virtual ~RTPRelaySink();

	static void afterGettingFrame(void* clientData, unsigned frameSize,
						unsigned numTruncatedBytes,
						struct timeval presentationTime,
						unsigned durationInMicroseconds);
	void afterGettingFrame(unsigned frameSize, unsigned numTruncatedBytes,
						struct timeval presentationTime, unsigned durationInMicroseconds);

	static void afterGettingPacket(void* clientData, char* packetData, unsigned packetSize);
	void afterGettingPacket(char* packetData, unsigned packetSize);

private:
	// redefined virtual functions:
	virtual Boolean continuePlaying();

private:
	u_int8_t*        fReceiveBuffer;
	MediaSubsession& fSubsession;
	char*            fStreamId;
	u_int16_t        fLastRTPSeq;
	LiveRTSPSession& fLiveRTSPSession;
	int              fTrackIndex;
};

#endif
