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
// A test program that receives a RTP/RTCP multicast MPEG-2 Transport Stream,
// and send the resulting Transport Stream data to a new multicast 
// main program

#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"
#include "GroupsockHelper.hh"


#define TRANSPORT_PACKET_SIZE 188
#define TRANSPORT_PACKETS_PER_NETWORK_PACKET 7

void afterPlaying(void* clientData); // forward

// A structure to hold the state of the current session.
// It is used in the "afterPlaying()" function to clean up the session.
struct SessionState_t {
  RTPSource* videoSource;
  MediaSink* videoSink;
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
  // Create 'groupsocks' for UDP sender:
  struct in_addr sendAddress;
  sendAddress.s_addr = our_inet_addr("239.0.0.1");
  const Port sendPort(5004);
  Groupsock sendGroupsock(*env, sendAddress, sendPort, 1);

  // Create the data sink : a MPEG-2 TranksportStream UDP sink
  sessionState.videoSink = BasicUDPSink::createNew(*env, &sendGroupsock, TRANSPORT_PACKET_SIZE * TRANSPORT_PACKETS_PER_NETWORK_PACKET);

  /*
   * 2. Setup UDP receiver.
   */
  // Create 'groupsocks' for RTP and RTCP receiver:
  struct in_addr receiveAddress;
  receiveAddress.s_addr = our_inet_addr("239.255.42.42");
  const Port rtpPort(1234);
  const Port rtcpPort(1235);
  Groupsock rtpGroupsock(*env, receiveAddress, rtpPort, 1);
  Groupsock rtcpGroupsock(*env, receiveAddress, rtcpPort, 1);

  // Create the data source: a MPEG-2 TransportStream RTP source (which uses a 'simple' RTP payload format):
  sessionState.videoSource = SimpleRTPSource::createNew(*env, &rtpGroupsock, 33, 90000, "video/MP2T", 0, False /*no 'M' bit*/);

  // Create (and start) a 'RTCP instance' for the RTP source:
  const unsigned estimatedSessionBandwidth = 5000; // in kbps; for RTCP b/w share
  const unsigned maxCNAMElen = 100;
  unsigned char CNAME[maxCNAMElen+1];
  gethostname((char*)CNAME, maxCNAMElen);
  CNAME[maxCNAMElen] = '\0'; // just in case
  sessionState.rtcpInstance =
    RTCPInstance::createNew(*env, &rtcpGroupsock,
          estimatedSessionBandwidth, CNAME,
          NULL /* we're a client */, sessionState.videoSource);
  // Note: This starts RTCP running automatically

  /*
   * 3. Finally, start playing.
   */
  *env << "Beginning reading on \""  << rtpGroupsock << "\"\n";
  sessionState.videoSink->startPlaying(*sessionState.videoSource, afterPlaying, NULL);

  env->taskScheduler().doEventLoop(); // does not return

  return 0; // only to prevent compiler warning
}


void afterPlaying(void* /*clientData*/) {
  *env << "...done reading\n";

  // End by closing the media:
  Medium::close(sessionState.rtcpInstance); // Note: Sends a RTCP BYE
  Medium::close(sessionState.videoSink);
  Medium::close(sessionState.videoSource);
}
