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

#define PACKET_MEMORY_LENGTH 1024

// Keep a list of checksums that we have seen recently.
static uint32_t seen_checksums[PACKET_MEMORY_LENGTH];

//typedef struct
//{
//	int dest;
//	char * data;
//}, message;

//static int first = 0;
//static int last = 0;
//static struct message buffer[100];

static int CAN_SEND = 0;

// The next index we should overwrite in seen_checksums.
static size_t next_seen_checksum = 0;

/// Will print out a message, couldnt this be done during the up_from_dll instead of creating a whole new function??.
void sendNext() {
        printf("ENTERING SEND NEXT MODE");
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
 
  uint32_t checksum = packet.checksum;
  packet.checksum = 0;
   
  // Ensure checksum is valid.
  if (CNET_crc32((unsigned char *)&packet, sizeof(packet)) != checksum) {
    printf("\tChecksum failed.\n");
    return;
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
    .length = NL_MAXDATA
  };

  // Create a RTS packet.
  struct nl_packet rts = (struct nl_packet){
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
  
 // Create a broadcast address.
  CnetNICaddr wifi_dest;
  CHECK(CNET_parse_nicaddr(wifi_dest, "ff:ff:ff:ff:ff:ff"));
  //CnetNICaddr rts_dest; 
  //CHECK(CNET_parse_nicaddr(rts_dest, "ff:ff:ff:ff:ff:ff"));
 
  uint16_t packet_length = NL_PACKET_LENGTH(packet);

  //printf("RTS DATA: %s\n", rts.data);

  //printf("packt.data: %s\n", packet.data); 
  //printf("Packet size: %zu\n", sizeof(packet));
  printf("wifi dest: %zu\n", packet.dest);
  //printf("Node info: %zu\n", nodeinfo.nlinks);

  for (int i = 1; i <= nodeinfo.nlinks; ++i) {
    if (dll_states[i] != NULL) {
       dll_wifi_write(dll_states[i], wifi_dest, (char *)&packet, packet_length); 

	//message{
	//	.dest = packet.dest;
	//	.data = packet.data;
	//};
	//dll_wifi_write(dll_states[i], wifi_dest, (char *)&packet, packet_length);
    }
  }
}

/// Called when this mobile node is booted up.
///
void reboot_mobile() {
  // We require each node to have a different stream of random numbers.
  CNET_srand(nodeinfo.time_of_day.sec + nodeinfo.nodenumber);

  // Provide the required event handlers.
  CHECK(CNET_set_handler(EV_PHYSICALREADY, physical_ready, 0));
  CHECK(CNET_set_handler(EV_APPLICATIONREADY, application_ready, 0));

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
