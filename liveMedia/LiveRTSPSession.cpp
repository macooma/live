#include "LiveRTSPSession.hh"

#define RTSP_CLIENT_VERBOSITY_LEVEL 1
#define REQUEST_STREAMING_OVER_TCP True
// Even though we're not going to be doing anything with the incoming data, we still need to receive it.
// Define the size of the buffer that we'll use:
#define DUMMY_SINK_RECEIVE_BUFFER_SIZE 1000000
// If you don't want to see debugging output for each received frame, then comment out the following line:
//#define DEBUG_PRINT_EACH_RECEIVED_FRAME 1

#define Assert(condition)    {                              \
                                                            \
    if (!(condition))                                       \
    {                                                       \
        printf("_Assert: %s, %d\n", __FILE__, __LINE__ );   \
        (*(int*)0) = 0;                                     \
    }   }


LiveRTSPSession* LiveRTSPSession::createNew(char const* rtspURL, char const* progName)
{
	return new LiveRTSPSession(rtspURL, progName);
}

LiveRTSPSession::LiveRTSPSession(char const* rtspURL, char const* progName)
		: fRTSPClient(NULL),
		fScheduler(NULL), fEnv(NULL),
		fOnGettingSDPFunc(NULL),
		fOnGettingPacketFunc(NULL),
		fCallbackData(NULL),
		fThreadWatchVariable(0),
		fThreadStarted(false)
{
	fScheduler = BasicTaskScheduler::createNew();
	fEnv = BasicUsageEnvironment::createNew(*fScheduler);

	Assert(kMaxURLLen >= strlen(rtspURL) + 1);
	memcpy(fURL, rtspURL, strlen(rtspURL) + 1);

	fRTSPClient = LiveRTSPClient::createNew(*fEnv, rtspURL, *this, RTSP_CLIENT_VERBOSITY_LEVEL, progName);
	if (fRTSPClient == NULL) {
		*fEnv << "Failed to create a RTSP client for URL \"" << rtspURL << "\": " << fEnv->getResultMsg() << "\n";
		return;
	}
}

LiveRTSPSession::~LiveRTSPSession()
{
	if (fRTSPClient != NULL) {
		shutdownStream(fRTSPClient);
		// fRTSPClient has been reclaimed in shutdownStream()
		fRTSPClient = NULL;
	}

	StopAndWait();

	fEnv->reclaim(); fEnv = NULL;
	delete fScheduler; fScheduler = NULL;
}

int LiveRTSPSession::Start(OnGettingSDPFunc* onGettingSDPFunc, OnGettingPacketFunc* onGettingPacketFunc, void* callbackData)
{
	fOnGettingSDPFunc = onGettingSDPFunc;
	fOnGettingPacketFunc = onGettingPacketFunc;
	fCallbackData = callbackData;

	if (!fThreadStarted) {
		// start thread
		int err = pthread_create((pthread_t*)&fThread, NULL, thread_entry, (void*)this);
		if (err != 0) {
			*fEnv << "Failed to create live thread (%s)\n" << strerror(errno);
			return err;
		}

		fThreadStarted = true;
	}

	return 0;
}

void LiveRTSPSession::StopAndWait()
{
	void *retVal;

	if (fThreadWatchVariable == 0 && fThreadStarted) {
		fThreadWatchVariable = 1;

		pthread_join(fThread, &retVal); // wait for thread
	}
}

void LiveRTSPSession::NotifySDPInfo(char* const sdpInfo)
{
	Assert(kMaxSDPLen >= strlen(sdpInfo) + 1);
	memcpy(fSDPInfo, sdpInfo, strlen(sdpInfo) + 1);

	if (fOnGettingSDPFunc != NULL) {
		(*fOnGettingSDPFunc)(fCallbackData, sdpInfo);
	}
}

void LiveRTSPSession::NotifyPacket(char* packetData, unsigned packetSize, int trackIndex)
{
	if (fOnGettingPacketFunc != NULL) {
		(*fOnGettingPacketFunc)(fCallbackData, packetData, packetSize, trackIndex);
	}
}

// A function that outputs a string that identifies each stream (for debugging output).  Modify this if you wish:
UsageEnvironment& operator<<(UsageEnvironment& env, const RTSPClient& rtspClient) {
	return env << "[URL:\"" << rtspClient.url() << "\"]: ";
}

// A function that outputs a string that identifies each subsession (for debugging output).  Modify this if you wish:
UsageEnvironment& operator<<(UsageEnvironment& env, const MediaSubsession& subsession) {
	return env << subsession.mediumName() << "/" << subsession.codecName();
}

void* LiveRTSPSession::thread_entry(void* session)
{
	LiveRTSPSession* thiz = (LiveRTSPSession*)session;

	thiz->fRTSPClient->sendDescribeCommand(continueAfterDESCRIBE);
	thiz->fEnv->taskScheduler().doEventLoop(&thiz->fThreadWatchVariable);
	thiz->fThreadStarted = false;
	*(thiz->fEnv) << "[" << thiz->GetURL() << "] thread finished.\n";

	return NULL;
}

// Implementation of the RTSP 'response handlers':
// static
void LiveRTSPSession::continueAfterDESCRIBE(RTSPClient* rtspClient, int resultCode, char* resultString) {
	do {
		UsageEnvironment& env = rtspClient->envir(); // alias
		StreamClientState& scs = ((LiveRTSPClient*)rtspClient)->scs; // alias

		if (resultCode != 0) {
			env << *rtspClient << "Failed to get a SDP description: " << resultString << "\n";
			delete[] resultString;
			break;
		}

		char* const sdpDescription = resultString;
		env << *rtspClient << "Got a SDP description:\n" << sdpDescription << "\n";

		// notify of SDP information
		((LiveRTSPClient*)rtspClient)->liveRTSPSession().NotifySDPInfo(sdpDescription);

		// Create a media session object from this SDP description:
		scs.session = MediaSession::createNew(env, sdpDescription);
		delete[] sdpDescription; // because we don't need it anymore
		if (scs.session == NULL) {
			env << *rtspClient << "Failed to create a MediaSession object from the SDP description: " << env.getResultMsg() << "\n";
			break;
		} else if (!scs.session->hasSubsessions()) {
			env << *rtspClient << "This session has no media subsessions (i.e., no \"m=\" lines)\n";
			break;
		}

		// Then, create and set up our data source objects for the session.  We do this by iterating over the session's 'subsessions',
		// calling "MediaSubsession::initiate()", and then sending a RTSP "SETUP" command, on each one.
		// (Each 'subsession' will have its own data source.)
		scs.iter = new MediaSubsessionIterator(*scs.session);
		setupNextSubsession(rtspClient);
		return;
	} while (0);

	// An unrecoverable error occurred with this stream.
	shutdownStream(rtspClient);
}

// static
void LiveRTSPSession::setupNextSubsession(RTSPClient* rtspClient) {
	UsageEnvironment& env = rtspClient->envir(); // alias
	StreamClientState& scs = ((LiveRTSPClient*)rtspClient)->scs; // alias

	scs.subsession = scs.iter->next();
	if (scs.subsession != NULL) {
		if (!scs.subsession->initiate()) {
			env << *rtspClient << "Failed to initiate the \"" << *scs.subsession << "\" subsession: " << env.getResultMsg() << "\n";
			setupNextSubsession(rtspClient); // give up on this subsession; go to the next one
		} else {
			env << *rtspClient << "Initiated the \"" << *scs.subsession << "\" subsession (";
			if (scs.subsession->rtcpIsMuxed()) {
				env << "client port " << scs.subsession->clientPortNum();
			} else {
				env << "client ports " << scs.subsession->clientPortNum() << "-" << scs.subsession->clientPortNum()+1;
			}
			env << ")\n";

			// Continue setting up this subsession, by sending a RTSP "SETUP" command:
			rtspClient->sendSetupCommand(*scs.subsession, continueAfterSETUP, False, REQUEST_STREAMING_OVER_TCP);
		}
		return;
	}

	// We've finished setting up all of the subsessions.  Now, send a RTSP "PLAY" command to start the streaming:
	if (scs.session->absStartTime() != NULL) {
	// Special case: The stream is indexed by 'absolute' time, so send an appropriate "PLAY" command:
	rtspClient->sendPlayCommand(*scs.session, continueAfterPLAY, scs.session->absStartTime(), scs.session->absEndTime());
	} else {
	scs.duration = scs.session->playEndTime() - scs.session->playStartTime();
	rtspClient->sendPlayCommand(*scs.session, continueAfterPLAY);
	}
}

// static
void LiveRTSPSession::continueAfterSETUP(RTSPClient* rtspClient, int resultCode, char* resultString) {
	do {
		UsageEnvironment& env = rtspClient->envir(); // alias
		StreamClientState& scs = ((LiveRTSPClient*)rtspClient)->scs; // alias

		if (resultCode != 0) {
			env << *rtspClient << "Failed to set up the \"" << *scs.subsession << "\" subsession: " << resultString << "\n";
			break;
		}

		env << *rtspClient << "Set up the \"" << *scs.subsession << "\" subsession (";
		if (scs.subsession->rtcpIsMuxed()) {
			env << "client port " << scs.subsession->clientPortNum();
		} else {
			env << "client ports " << scs.subsession->clientPortNum() << "-" << scs.subsession->clientPortNum()+1;
		}
		env << ")\n";

		// Having successfully setup the subsession, create a data sink for it, and call "startPlaying()" on it.
		// (This will prepare the data sink to receive data; the actual flow of data from the client won't start happening until later,
		// after we've sent a RTSP "PLAY" command.)

		scs.subsession->sink = RTPRelaySink::createNew(env, *scs.subsession,
				((LiveRTSPClient*)rtspClient)->liveRTSPSession(), rtspClient->url());
			// perhaps use your own custom "MediaSink" subclass instead
		if (scs.subsession->sink == NULL) {
			env << *rtspClient << "Failed to create a data sink for the \"" << *scs.subsession
			<< "\" subsession: " << env.getResultMsg() << "\n";
			break;
		}

		env << *rtspClient << "Created a data sink for the \"" << *scs.subsession << "\" subsession\n";
		scs.subsession->miscPtr = rtspClient; // a hack to let subsession handler functions get the "RTSPClient" from the subsession 
		scs.subsession->sink->startPlaying(*(scs.subsession->readSource()),
							subsessionAfterPlaying, scs.subsession);
		// Also set a handler to be called if a RTCP "BYE" arrives for this subsession:
		if (scs.subsession->rtcpInstance() != NULL) {
			scs.subsession->rtcpInstance()->setByeHandler(subsessionByeHandler, scs.subsession);
		}
	} while (0);
	delete[] resultString;

	// Set up the next subsession, if any:
	setupNextSubsession(rtspClient);
}

// static
void LiveRTSPSession::continueAfterPLAY(RTSPClient* rtspClient, int resultCode, char* resultString) {
	Boolean success = False;

	do {
		UsageEnvironment& env = rtspClient->envir(); // alias
		StreamClientState& scs = ((LiveRTSPClient*)rtspClient)->scs; // alias

		if (resultCode != 0) {
			env << *rtspClient << "Failed to start playing session: " << resultString << "\n";
			break;
		}

		// Set a timer to be handled at the end of the stream's expected duration (if the stream does not already signal its end
		// using a RTCP "BYE").  This is optional.  If, instead, you want to keep the stream active - e.g., so you can later
		// 'seek' back within it and do another RTSP "PLAY" - then you can omit this code.
		// (Alternatively, if you don't want to receive the entire stream, you could set this timer for some shorter value.)
		if (scs.duration > 0) {
			unsigned const delaySlop = 2; // number of seconds extra to delay, after the stream's expected duration.  (This is optional.)
			scs.duration += delaySlop;
			unsigned uSecsToDelay = (unsigned)(scs.duration*1000000);
			scs.streamTimerTask = env.taskScheduler().scheduleDelayedTask(uSecsToDelay, (TaskFunc*)streamTimerHandler, rtspClient);
		}

		env << *rtspClient << "Started playing session";
		if (scs.duration > 0) {
			env << " (for up to " << scs.duration << " seconds)";
		}
		env << "...\n";

		success = True;
	} while (0);
	delete[] resultString;

	if (!success) {
		// An unrecoverable error occurred with this stream.
		shutdownStream(rtspClient);
	}
}


// Implementation of the other event handlers:
// static
void LiveRTSPSession::subsessionAfterPlaying(void* clientData) {
	MediaSubsession* subsession = (MediaSubsession*)clientData;
	RTSPClient* rtspClient = (RTSPClient*)(subsession->miscPtr);

	// Begin by closing this subsession's stream:
	Medium::close(subsession->sink);
	subsession->sink = NULL;

	// Next, check whether *all* subsessions' streams have now been closed:
	MediaSession& session = subsession->parentSession();
	MediaSubsessionIterator iter(session);
	while ((subsession = iter.next()) != NULL) {
		if (subsession->sink != NULL) return; // this subsession is still active
	}

	// All subsessions' streams have now been closed, so shutdown the client:
	shutdownStream(rtspClient);
}

// static
void LiveRTSPSession::subsessionByeHandler(void* clientData) {
	MediaSubsession* subsession = (MediaSubsession*)clientData;
	RTSPClient* rtspClient = (RTSPClient*)subsession->miscPtr;
	UsageEnvironment& env = rtspClient->envir(); // alias

	env << *rtspClient << "Received RTCP \"BYE\" on \"" << *subsession << "\" subsession\n";

	// Now act as if the subsession had closed:
	subsessionAfterPlaying(subsession);
}

// static
void LiveRTSPSession::streamTimerHandler(void* clientData) {
	LiveRTSPClient* rtspClient = (LiveRTSPClient*)clientData;
	StreamClientState& scs = rtspClient->scs; // alias

	scs.streamTimerTask = NULL;

	// Shut down the stream:
	shutdownStream(rtspClient);
}

// static
void LiveRTSPSession::shutdownStream(RTSPClient* rtspClient, int exitCode) {
	UsageEnvironment& env = rtspClient->envir(); // alias
	StreamClientState& scs = ((LiveRTSPClient*)rtspClient)->scs; // alias
	LiveRTSPSession& live = ((LiveRTSPClient*)rtspClient)->liveRTSPSession();

	// First, check whether any subsessions have still to be closed:
	if (scs.session != NULL) { 
		Boolean someSubsessionsWereActive = False;
		MediaSubsessionIterator iter(*scs.session);
		MediaSubsession* subsession;

		while ((subsession = iter.next()) != NULL) {
			if (subsession->sink != NULL) {
				Medium::close(subsession->sink);
				subsession->sink = NULL;

				if (subsession->rtcpInstance() != NULL) {
					subsession->rtcpInstance()->setByeHandler(NULL, NULL); // in case the server sends a RTCP "BYE" while handling "TEARDOWN"
				}

				someSubsessionsWereActive = True;
			}
		}

		if (someSubsessionsWereActive) {
			// Send a RTSP "TEARDOWN" command, to tell the server to shutdown the stream.
			// Don't bother handling the response to the "TEARDOWN".
			rtspClient->sendTeardownCommand(*scs.session, NULL);
		}
	}

	env << *rtspClient << "Closing the stream.\n";
	Medium::close(rtspClient);
	// Note that this will also cause this stream's "StreamClientState" structure to get reclaimed.

	// fRTSPClient has been deconstructed by Medium::close
	live.fRTSPClient = NULL;
	live.fThreadWatchVariable = 1;
}

// Implementation of "LiveRTSPClient":
// static
LiveRTSPClient* LiveRTSPClient::createNew(UsageEnvironment& env, char const* rtspURL,
					LiveRTSPSession& liveRTSPSession,
					int verbosityLevel, char const* applicationName) {
	return new LiveRTSPClient(env, rtspURL, liveRTSPSession, verbosityLevel, applicationName);
}

LiveRTSPClient::LiveRTSPClient(UsageEnvironment& env, char const* rtspURL,
					LiveRTSPSession& liveRTSPSession,
					int verbosityLevel, char const* applicationName)
	: RTSPClient(env, rtspURL, verbosityLevel, applicationName, 0, -1),
	  fLiveRTSPSession(liveRTSPSession) {
}

LiveRTSPClient::~LiveRTSPClient() {
	envir() << ">>> LiveRTSPClient [" << url() << "] Deconstructed <<<\n";
}


// Implementation of "StreamClientState":
StreamClientState::StreamClientState()
	: iter(NULL), session(NULL), subsession(NULL), streamTimerTask(NULL), duration(0.0) {
}

StreamClientState::~StreamClientState() {
	delete iter;
	if (session != NULL) {
		// We also need to delete "session", and unschedule "streamTimerTask" (if set)
		UsageEnvironment& env = session->envir(); // alias

		env.taskScheduler().unscheduleDelayedTask(streamTimerTask);
		Medium::close(session);
	}
}


// Implementation of "RTPRelaySink":
// static
RTPRelaySink* RTPRelaySink::createNew(UsageEnvironment& env, MediaSubsession& subsession, LiveRTSPSession& liveRTSPSession, char const* streamId) {
	return new RTPRelaySink(env, subsession, liveRTSPSession, streamId);
}

RTPRelaySink::RTPRelaySink(UsageEnvironment& env, MediaSubsession& subsession, LiveRTSPSession& liveRTSPSession, char const* streamId)
	: MediaSink(env),
	fSubsession(subsession),
	fLastRTPSeq(0),
	fLiveRTSPSession(liveRTSPSession) {
	fStreamId = strDup(streamId);
	fReceiveBuffer = new u_int8_t[DUMMY_SINK_RECEIVE_BUFFER_SIZE];

	MediaSession& session = subsession.parentSession();
	MediaSubsessionIterator iter(session);
	MediaSubsession* tmp;
	int index = 0;
	while ((tmp = iter.next()) != NULL) {
		if (tmp == &subsession)
			break;
		index++;
	}
	fTrackIndex = index;
}

RTPRelaySink::~RTPRelaySink() {
	delete[] fReceiveBuffer;
	delete[] fStreamId;
}

// static
void RTPRelaySink::afterGettingFrame(void* clientData, unsigned frameSize, unsigned numTruncatedBytes,
				  struct timeval presentationTime, unsigned durationInMicroseconds) {
	RTPRelaySink* sink = (RTPRelaySink*)clientData;
	sink->afterGettingFrame(frameSize, numTruncatedBytes, presentationTime, durationInMicroseconds);
}

void RTPRelaySink::afterGettingFrame(unsigned frameSize, unsigned numTruncatedBytes,
				  struct timeval /*presentationTime*/, unsigned /*durationInMicroseconds*/) {
	// We've just received a frame of data.  (Optionally) print out information about it:
#ifdef DEBUG_PRINT_EACH_RECEIVED_FRAME
	if (fStreamId != NULL) envir() << "Stream \"" << fStreamId << "\"; ";
	envir() << fSubsession.mediumName() << "/" << fSubsession.codecName() << ":\tReceived " << frameSize << " bytes";
	if (numTruncatedBytes > 0) envir() << " (with " << numTruncatedBytes << " bytes truncated)";
	envir() << "\n";
#endif

	// Then continue, to request the next frame of data:
	continuePlaying();
}

// static
void RTPRelaySink::afterGettingPacket(void* clientData, char* packetData, unsigned packetSize)
{
	RTPRelaySink* sink = (RTPRelaySink*)clientData;
	sink->afterGettingPacket(packetData, packetSize);
}

void RTPRelaySink::afterGettingPacket(char* packetData, unsigned packetSize)
{
	// notify of RTP packet
	fLiveRTSPSession.NotifyPacket(packetData, packetSize, fTrackIndex);

	{
		u_int16_t* seqNumPtr = (u_int16_t*)packetData;

		if (fLastRTPSeq != 0) {
			if (ntohs(seqNumPtr[1]) - fLastRTPSeq > 1) {
				printf("::: %u rec packets lost :::\n", ntohs(seqNumPtr[1]) - fLastRTPSeq - 1);
			}
		}
		fLastRTPSeq = ntohs(seqNumPtr[1]);

		//if (ntohs(seqNumPtr[1]) % 100 == 0)
		//	printf("Rec RTP seq: %u\n", ntohs(seqNumPtr[1]));
	}
  
}

Boolean RTPRelaySink::continuePlaying() {
	if (fSource == NULL) return False; // sanity check (should not happen)

	// Request the next frame of data from our input source.  "afterGettingFrame()" will get called later, when it arrives:
	fSource->getNextFrame(fReceiveBuffer, DUMMY_SINK_RECEIVE_BUFFER_SIZE,
						afterGettingFrame, this,
						onSourceClosure, this,
						afterGettingPacket);
	return True;
}
