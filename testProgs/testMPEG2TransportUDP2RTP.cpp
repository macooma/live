/**********
This library is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the
Free Software Foundation; either version 3 of the License, or (at your
option) any later version. (See <http://www.gnu.org/copyleft/lesser.html>.)

This library is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
more details.

You should have received a copy of the GNU Lesser General Public License
along with this library; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
**********/
// Copyright (c) 1996-2017, Live Networks, Inc.  All rights reserved
// A test program that receives a MPEG-2 Transport Stream from multicast,
// and send it using RTP/RTCP to a new multicast
// main program

#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"
#include "GroupsockHelper.hh"
#include "MPEG2TransportStreamAccumulator.hh"

#define TRANSPORT_PACKET_SIZE 188
#define TRANSPORT_PACKETS_PER_NETWORK_PACKET 7

// To set up an internal RTSP server, uncomment the following:
//#define IMPLEMENT_RTSP_SERVER
// (Note that this RTSP server works for multicast only)

#ifdef IMPLEMENT_RTSP_SERVER
void initRTSPServer();
#endif

void afterPlaying(void* clientData); // forward

// A structure to hold the state of the current session.
// It is used in the "afterPlaying()" function to clean up the session.
struct SessionState_t {
  FramedSource* videoSource;
  RTPSink*      videoSink;
  RTCPInstance* rtcpInstance;
} sessionState;

UsageEnvironment* env;

int main(int argc, char** argv) {
  // Begin by setting up our usage environment:
  TaskScheduler* scheduler = BasicTaskScheduler::createNew();
  env = BasicUsageEnvironment::createNew(*scheduler);

  /*
   * 1. Setup UDP sender.
   */
  // Create 'groupsocks' for RTP and RTCP sender:
  struct in_addr sendAddress;
  sendAddress.s_addr = our_inet_addr("239.255.42.42");
  const Port rtpPort(1234);
  const Port rtcpPort(1235);
  Groupsock rtpGroupsock(*env, sendAddress, rtpPort, 7 /* ttl */);
  Groupsock rtcpGroupsock(*env, sendAddress, rtcpPort, 7 /* ttl */);

  // Create a data sink: a MPEG-2 TransportStream RTP sink (which uses a 'simple' RTP payload format):
  sessionState.videoSink = SimpleRTPSink::createNew(*env, &rtpGroupsock, 33, 90000, "video", "MP2T", 1, True, False /*no 'M' bit*/);

  // Create (and start) a 'RTCP instance' for this RTP sink:
  const unsigned estimatedSessionBandwidth = 5000; // in kbps; for RTCP b/w share
  const unsigned maxCNAMElen = 100;
  unsigned char CNAME[maxCNAMElen+1];
  gethostname((char*)CNAME, maxCNAMElen);
  CNAME[maxCNAMElen] = '\0'; // just in case

  sessionState.rtcpInstance =
    RTCPInstance::createNew(*env, &rtcpGroupsock,
          estimatedSessionBandwidth, CNAME,
          sessionState.videoSink, NULL /* we're a server */, False);
  // Note: This starts RTCP running automatically

  /*
   * 2. Setup RTSP server if needed.
   */
#ifdef IMPLEMENT_RTSP_SERVER
  initRTSPServer();
#endif

  /*
   * 3. Setup UDP receiver.
   */
  // Create 'groupsocks' for UDP receiver:
  struct in_addr receiveAddress;
  receiveAddress.s_addr = our_inet_addr("239.0.0.1");
  const Port receivePort(5004);
  Groupsock receiveGroupsock(*env, receiveAddress, receivePort, 7 /* ttl */);

  // Create data source: a MPEG-2 TransportStream UDP source
  BasicUDPSource* udpSource = BasicUDPSource::createNew(*env, &receiveGroupsock);
  if (udpSource == NULL) {
    *env << "Unable to create udp source on \"" << receiveGroupsock << "\"\n";
    exit(1);
  }

  // Create a 'framer' for the input source (to give us proper inter-packet gaps):
  sessionState.videoSource = MPEG2TransportStreamAccumulator::createNew(*env, udpSource,
                    TRANSPORT_PACKET_SIZE * TRANSPORT_PACKETS_PER_NETWORK_PACKET);

  /*
   * 4. Finally, start playing.
   */
  *env << "Beginning reading on \""  << receiveGroupsock << "\"\n";
  sessionState.videoSink->startPlaying(*sessionState.videoSource, afterPlaying, sessionState.videoSink);

  env->taskScheduler().doEventLoop(); // does not return

  return 0; // only to prevent compiler warning
}

void afterPlaying(void* /*clientData*/) {
  *env << "...done reading\n";

  // End by closing the media:
  Medium::close(sessionState.rtcpInstance);
  Medium::close(sessionState.videoSink);
  Medium::close(sessionState.videoSource);
  // Note that this also closes the udp source that this source read from.
}

void initRTSPServer() {
  RTSPServer* rtspServer = RTSPServer::createNew(*env, Port(1554));
  if (rtspServer == NULL) {
    *env << "Failed to create RTSP server: " << env->getResultMsg() << "\n";
    exit(1);
  }
  ServerMediaSession* sms
    = ServerMediaSession::createNew(*env, "MPEG2-TS", "MPEG2 Transport Stream", "MPEG2 Transport Stream Session", False);
  sms->addSubsession(PassiveServerMediaSubsession::createNew(*sessionState.videoSink, sessionState.rtcpInstance));
  rtspServer->addServerMediaSession(sms);

  char* url = rtspServer->rtspURL(sms);
  *env << "Play this stream using the URL \"" << url << "\"\n";
  delete[] url;
}