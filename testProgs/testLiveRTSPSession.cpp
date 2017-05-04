#include "LiveRTSPSession.hh"

void OnGettingSDPFunc(void* callbackData, char* sdpInfo)
{
	printf("received sdp: %s\n", sdpInfo);
}
void OnGettingPacketFunc(void* callbackData, char* packetData,
					unsigned packetSize, int trackIndex)
{
	u_int16_t* seqNumPtr = (u_int16_t*)packetData;
	printf("received packet seq=%u, track=%d, size=%u\n", ntohs(seqNumPtr[1]), trackIndex, packetSize);
}

int main(int argc, char** argv) {
	LiveRTSPSession *session = LiveRTSPSession::createNew("rtsp://192.168.199.157:8554/h264.mkv",
		"LiveRTSPSession");

	session->Start(OnGettingSDPFunc, OnGettingPacketFunc, NULL);
	sleep(10);

	session->StopAndWait();
	delete session;
}
