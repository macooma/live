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
// A test program that reads a MPEG-2 Transport Stream file,
// and streams it using RTP
// main program

#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"
#include "GroupsockHelper.hh"


// To set up an internal RTSP server, uncomment the following:
#define IMPLEMENT_RTSP_SERVER 1
// (Note that this RTSP server works for multicast only)

#define TRANSPORT_PACKET_SIZE 188
#define TRANSPORT_PACKETS_PER_NETWORK_PACKET 7
// The product of these two numbers must be enough to fit within a network packet

struct SessionState_t {
  FramedSource* videoSource;
  RTPSink*      videoSink;
  Groupsock*    rtpGroupsock;
  Groupsock*    rtcpGroupsock;
  Groupsock*    receivingGroupsock;
#ifdef IMPLEMENT_RTSP_SERVER
  RTCPInstance* rtcpInstance;
#endif
} sessionState;

UsageEnvironment* env;

void play(); // forward

int main(int argc, char** argv) {
  // Begin by setting up our usage environment:
  TaskScheduler* scheduler = BasicTaskScheduler::createNew();
  env = BasicUsageEnvironment::createNew(*scheduler);

  // Create 'groupsocks' for RTP and RTCP:
  struct in_addr destinationAddress;
  destinationAddress.s_addr = our_inet_addr("239.255.42.42");
  const Port rtpPort(1234);
  const Port rtcpPort(1235);
  sessionState.rtpGroupsock = new Groupsock(*env, destinationAddress, rtpPort, 7 /* ttl */);
  sessionState.rtcpGroupsock = new Groupsock(*env, destinationAddress, rtcpPort, 7 /* ttl */);

  // Create 'groupsocks' for receiving MPEG2-TS packets:
  struct in_addr broadcastAddress;
  broadcastAddress.s_addr = our_inet_addr("239.0.0.1");
  const Port receivePort(5004);
  sessionState.receivingGroupsock = new Groupsock(*env, broadcastAddress, receivePort, 7 /* ttl */);

  // Create an appropriate 'RTP sink' from the RTP 'groupsock':
  sessionState.videoSink = SimpleRTPSink::createNew(*env, sessionState.rtpGroupsock, 33, 90000, "video", "MP2T", 1, True, False /*no 'M' bit*/);

  // Create (and start) a 'RTCP instance' for this RTP sink:
  const unsigned estimatedSessionBandwidth = 5000; // in kbps; for RTCP b/w share
  const unsigned maxCNAMElen = 100;
  unsigned char CNAME[maxCNAMElen+1];
  gethostname((char*)CNAME, maxCNAMElen);
  CNAME[maxCNAMElen] = '\0'; // just in case

#ifdef IMPLEMENT_RTSP_SERVER
  sessionState.rtcpInstance =
#endif
  RTCPInstance::createNew(*env, sessionState.rtcpGroupsock,
			    estimatedSessionBandwidth, CNAME,
			    sessionState.videoSink, NULL /* we're a server */, False);
  // Note: This starts RTCP running automatically

#ifdef IMPLEMENT_RTSP_SERVER
  RTSPServer* rtspServer = RTSPServer::createNew(*env, Port(1554));
  // Note that this (attempts to) start a server on the default RTSP server
  // port: 554.  To use a different port number, add it as an extra
  // (optional) parameter to the "RTSPServer::createNew()" call above.
  if (rtspServer == NULL) {
    *env << "Failed to create RTSP server: " << env->getResultMsg() << "\n";
    exit(1);
  }
  ServerMediaSession* sms
    = ServerMediaSession::createNew(*env, "MPEG-TS", "MPEG2-TS relaying stream", "MPEG2-TS relaying stream", False);
  sms->addSubsession(PassiveServerMediaSubsession::createNew(*sessionState.videoSink, sessionState.rtcpInstance));
  rtspServer->addServerMediaSession(sms);

  char* url = rtspServer->rtspURL(sms);
  *env << "Play this stream using the URL \"" << url << "\"\n";
  delete[] url;
#endif

  // Finally, start the streaming:
  *env << "Beginning streaming...\n";
  play();

  env->taskScheduler().doEventLoop(); // does not return

  return 0; // only to prevent compiler warning
}

void afterPlaying(void* /*clientData*/) {
  *env << "...done reading from broadcast socket\n";

  sessionState.videoSink->stopPlaying();
  Medium::close(sessionState.videoSource);
  // Note that this also closes the udp source that this source read from.

  play();
}

void play() {
  BasicUDPSource* udpSource = BasicUDPSource::createNew(*env, sessionState.receivingGroupsock);

  if (udpSource == NULL) {
    *env << "Unable to create udp source on " << *sessionState.receivingGroupsock << "\n";
    exit(1);
  }

  // Create a 'framer' for the input source (to give us proper inter-packet gaps):
  sessionState.videoSource = MPEG2TransportStreamFramer::createNew(*env, udpSource);

  // Finally, start playing:
  *env << "Beginning to receive on \""  << *sessionState.receivingGroupsock << "\"\n";
  sessionState.videoSink->startPlaying(*sessionState.videoSource, afterPlaying, sessionState.videoSink);
}
