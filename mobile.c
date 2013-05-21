/// This file implements the functionality of our mobile nodes.

#include <cnet.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "dll_wifi.h"
#include "mapping.h"
#include "network.h"
#include "walking.h"

// Mobile nodes can only have WLAN links, so we always use the WiFi data link
// layer.
static struct dll_wifi_state **dll_states;

#define MAX_BUFFER 100;
struct nl_packet lastpacket;	// Define a variable to hold our last packet data.
static size_t lastlength = 0;	// Define a variabe to hold our length.
static CnetTimerID lasttimer3 = NULLTIMER;	// Will store our timer data.

static int nextSeqNum = 0;	// Will store our next sequence number.
static int seqNumExpected =0;	// Will store the expected sequence number.
static int ackexpected = 0;	// Will store the ACK expected.
//static  int             nextframetosend         = 0;
//static  int             frameexpected           = 0;

#define PACKET_MEMORY_LENGTH 1024

// Keep a list of checksums that we have seen recently.
static uint32_t seen_checksums[PACKET_MEMORY_LENGTH];

typedef struct MESSAGE
{
	int dest;
	char * data;
} MESSAGE;

static int first = 0;
static int last = 0;
static struct MESSAGE buffer[100];

static int CAN_SEND = 0;	// Will determine if we can send.

// The next index we should overwrite in seen_checksums.
static size_t next_seen_checksum = 0;

/// Will print out a message
void sendNext() {
  // Create a packet.
  struct nl_packet packet = (struct nl_packet){
    .src = nodeinfo.address,
    .length = NL_MAXDATA
  };

  strcpy(packet.data, buffer[first].data);
  first ++;
  packet.checksum = CNET_crc32((unsigned char *)&packet, sizeof(packet));

  CnetNICaddr wifi_dest;
  CHECK(CNET_parse_nicaddr(wifi_dest, "ff:ff:ff:ff:ff:ff"));

  uint16_t packet_length = NL_PACKET_LENGTH(packet);

  for (int i = 1; i <= nodeinfo.nlinks; ++i) {
    if (dll_states[i] != NULL) {
	fprintf(stdout, "Node %d has sent a package. \n", nodeinfo.address);                                                        
	dll_wifi_write(dll_states[i], wifi_dest, (char *)&packet, packet_length);
    }
  }

}

/// This function will handle our frame timeouts.
///
EVENT_HANDLER(timeouts) {
  //transmit_frame(&lastmsg, DL_DATA, lastlength, 1-nextframetosend);
  struct nl_packet packet = lastpacket;
 
  CnetNICaddr wifi_dest;
  CHECK(CNET_parse_nicaddr(wifi_dest, "ff:ff:ff:ff:ff:ff"));
  
  for (int i = 1; i <= nodeinfo.nlinks; ++i) {
    if (dll_states[i] != NULL) {
      dll_wifi_write(dll_states[i], wifi_dest, (char *)&packet, packet.length);
    }
  }
  printf("DATA re-transmitted, seq=%d\n",packet.seqNum);
}

/// Called when this mobile node receives a frame on any of its physical links.
///
static EVENT_HANDLER(physical_ready) {
  // First we read the frame from the physical layer.
  char frame[DLL_MTU];
  size_t length	= sizeof(frame);
  int link;

  CHECK(CNET_read_physical(&link, frame, &length));
  
  // Now we forward this information to the data link layer, if it exists.
  if (link > nodeinfo.nlinks || dll_states[link] == NULL) return;
  
  dll_wifi_read(dll_states[link], frame, length);
}

/// Called when we receive data from one of our data link layers.
///
static void up_from_dll(int link, const char *data, size_t length) {  
  // If length of data is greater than packet length then discard.
  if (length > sizeof(struct nl_packet)) {
    printf("Mobile: %zu is larger than a nl_packet! ignoring.\n", length);
    return;
  }
  
  // Treat this frame as a network layer packet.
  struct nl_packet packet;
  memset(&packet, 0, sizeof(packet));
  memcpy(&packet, data, length);

  printf("Mobile: Received frame from dll on link %d from node %" PRId32
         " for node %" PRId32 ".\n", link, packet.src, packet.dest);
     
  printf("Mobile: type %d seqNum %d \n", packet.type, packet.seqNum);
  printf("Mobile data: %s\n", data);
  
  // Hold our checksum.
  uint32_t checksum = packet.checksum;
  packet.checksum = 0;
  
  // Set our broadcast.
  CnetNICaddr wifi_dest;
  CHECK(CNET_parse_nicaddr(wifi_dest, "ff:ff:ff:ff:ff:ff"));
   
  // Ensure checksum is valid.
  if((CNET_crc32((unsigned char *)&packet, sizeof(packet)) != checksum) || (packet.seqNum != seqNumExpected)) {
    printf("\tChecksum failed or wrong seqNum received:%d expected: %d \n", packet.seqNum, seqNumExpected);
    // Put something to send NACK here.
    struct nl_packet packet = (struct nl_packet) {
      .src = nodeinfo.address,
      .dest = packet.src,
      .length = 0,
      .type = NACK,
      .seqNum = packet.seqNum /// global next seqNum
    };
      
    //uint16_t packet_length = NL_PACKET_LENGTH(packet);
    //dll_wifi_write(dll_states[link], wifi_dest, (char *)&packet, packet_length);
    //printf("NACK transmitted, seq=%d\n",packet.seqNum);
    return;
  } 
  else {
    switch(packet.type) {
      case ACK: {
        if(packet.seqNum == ackexpected) {
          printf("\t\t\t\tACK received, seq=%d\n", packet.seqNum);
          CNET_stop_timer(lasttimer3);
          ackexpected = 1-ackexpected;
          //CNET_enable_application(ALLNODES);
        }
        break;
      }
      case NACK: {
        if(packet.seqNum == ackexpected) {
          printf("\t\t\t\tNACK received, seq=%d\n", packet.seqNum);
          CNET_stop_timer(lasttimer3);
          printf("timeout, seq=%d\n", ackexpected);
          //transmit_frame(lastmsg, DL_DATA, lastlength, ackexpected);
        }
        break;
      }
      case DATA: {
        printf("\t\t\t\tDATA received, seq=%d, ", packet.seqNum);
        if(packet.seqNum == seqNumExpected) {
          printf("up to application\n");
          //len = f.len;
          //CHECK(CNET_write_application(&f.msg, &len));
          seqNumExpected = 1-seqNumExpected;
        }
        else printf("ignored\n");
              
        struct nl_packet packet = (struct nl_packet){
          .src = nodeinfo.address,
          .dest = packet.src,
          .length = 0,
          .type = ACK,
          .seqNum = packet.seqNum /// global next seqNum
        };
              
        uint16_t packet_length = NL_PACKET_LENGTH(packet);
        dll_wifi_write(dll_states[link], wifi_dest, (char *)&packet, packet_length);
        printf("ACK transmitted, seq=%d\n",packet.seqNum);
        break;
      }
    }
  }
  
  // If packet destination does not match our address then discard.
  if (packet.dest != nodeinfo.address) {
    printf("\tThat's not for me.\n");
    
    //check the first three letters to see if it's "CTS" 
    char a[4];
    strncpy(a, packet.data, 3);
    //fprintf(stdout, "node %d:data is %s\n", nodeinfo.address, a);	// Strncpy is not working RTS is not being assigned.

    //If it's a CTS get who has the CTS
    if(0 == strcmp(a, "CTS")) {
      char access [5];
      memcpy(access, &packet.data[3], 2);
      //fprintf(stdout, "node %d: %s:\n", nodeinfo.address, a);
	
      printf("CTS to: %s\n", access);
      if(nodeinfo.address == atoi(access)) {
        CAN_SEND = 1; 
        printf("I am cleared to send\n");
        sendNext();
      }  	
      else {
        CAN_SEND = -1;
        printf("I better not be sending\n");
      }
    }
    return;
  }  
  
  // Check if we've seen this packet recently.
  for (size_t i = 0; i < PACKET_MEMORY_LENGTH; ++i) {
    if (seen_checksums[i] == checksum) {
      printf("\tI seem to have seen this recently.\n");
      return;
    }
  }
  
  // Remember the checksum of this packet.
  seen_checksums[next_seen_checksum++] = checksum;
  next_seen_checksum %= PACKET_MEMORY_LENGTH;
  
  // Send this packet to the application layer.
  printf("\tUp to the application layer!\n");
  
  size_t payload_length = packet.length;
  CHECK(CNET_write_application(packet.data, &payload_length));
}

/// Called when this mobile node's application layer has generated a new
/// message.
///
static EVENT_HANDLER(application_ready) {
  // Create a packet.
  struct nl_packet packet = (struct nl_packet){
    .src = nodeinfo.address,
    .length = NL_MAXDATA,
    .type = DATA,
    .seqNum = 1-nextSeqNum /// global next seqNum
  };

  // Create a RTS packet.
  struct nl_packet rts = (struct nl_packet) {
    .src = nodeinfo.address,
    .length = NL_MAXDATA
  }; 
  
  CHECK(CNET_read_application(&packet.dest, packet.data, &packet.length));
 
  strcpy(rts.data, "RTS");
  //fprintf(stdout, "node %d %s:\n", nodeinfo. address, rts.data);
 
  // Create checksum for RTS
  rts.checksum = CNET_crc32((unsigned char*)&rts, sizeof(rts));
  packet.checksum = CNET_crc32((unsigned char *)&packet, sizeof(packet));
  
  printf("Mobile: Generated message for % " PRId32
         ", broadcasting on all data link layers\n",
         packet.dest);
   
  lastpacket = packet;
    
  // Create a broadcast address.
  CnetNICaddr wifi_dest;
  CHECK(CNET_parse_nicaddr(wifi_dest, "ff:ff:ff:ff:ff:ff"));
  //CnetNICaddr rts_dest; 
  //CHECK(CNET_parse_nicaddr(rts_dest, "ff:ff:ff:ff:ff:ff"));
 
  uint16_t packet_length = NL_PACKET_LENGTH(packet);
  lastlength = packet_length;
  //printf("RTS DATA: %s\n", rts.data);

  //printf("packt.data: %s\n", packet.data); 
  //printf("Packet size: %zu\n", sizeof(packet));
  printf("wifi dest: %zu\n", packet.dest);
  //printf("Node info: %zu\n", nodeinfo.nlinks);
  CnetTime timeout;
  timeout = (packet_length*8000000 / linkinfo[1].bandwidth) + /// fix this to expected average
  	linkinfo[1].propagationdelay;
  lasttimer3 = CNET_start_timer(EV_TIMER3, timeout, 0);
  
  for (int i = 1; i <= nodeinfo.nlinks; ++i) {
    if (dll_states[i] != NULL) {
	fprintf(stdout, "Node %d has RTS\n", nodeinfo.address);                                                        
        dll_wifi_write(dll_states[i], wifi_dest, (char *)&rts, packet_length); 

	MESSAGE m = {
		.dest = packet.dest,
		.data = packet.data
	};
	
	buffer[last] = m;
	last ++;
	
	//dll_wifi_write(dll_states[i], wifi_dest, (char *)&packet, packet_length);
    }
  }
    printf("DATA transmitted, seq=%d\n",packet.seqNum);
    nextSeqNum = 1 - nextSeqNum;
}

/// Called when this mobile node is booted up.
///
void reboot_mobile() {
  // We require each node to have a different stream of random numbers.
  CNET_srand(nodeinfo.time_of_day.sec + nodeinfo.nodenumber);

  // Provide the required event handlers.
  CHECK(CNET_set_handler(EV_PHYSICALREADY, physical_ready, 0));
  CHECK(CNET_set_handler(EV_APPLICATIONREADY, application_ready, 0));
  CHECK(CNET_set_handler(EV_TIMER3, timeouts, 0));
  
  // Initialize mobility.
  init_walking();
  start_walking();

  // Prepare to talk via our wireless connection.
  CNET_set_wlan_model(my_WLAN_model);
  
  // Setup our data link layer instances.
  dll_states = calloc(nodeinfo.nlinks + 1, sizeof(struct dll_wifi_state *));
  
  // Set our wifi states.
  for (int link = 1; link <= nodeinfo.nlinks; ++link) {
    if (linkinfo[link].linktype == LT_WLAN) {
      dll_states[link] = dll_wifi_new_state(link,
                                            up_from_dll,
                                            false /* is_ds */);
    }
  }
  
  // Start cnet's default application layer.
  CNET_enable_application(ALLNODES);
  
  printf("reboot_mobile() complete.\n");
  printf("\tMy address: %" PRId32 ".\n", nodeinfo.address);
}
