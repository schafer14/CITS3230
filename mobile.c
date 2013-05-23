/// This file implements the functionality of our mobile nodes.

#include <cnet.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

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
//static CnetTimerID lasttimer3 = NULLTIMER;	// Will store our timer data.

static int ackexpected = 0;	// Will store the ACK expected.
//static  int             nextframetosend         = 0;
//static  int             frameexpected           = 0;

#define PACKET_MEMORY_LENGTH 1024

#define   WINDOWSIZE   100                                     //// MUST BE BIG ENOUGH!!!!!!!!
static  struct nl_packet packetsSent[WINDOWSIZE];
//static  struct nl_packet toSendPackets[WINDOWSIZE];
#define   MAXSEQ   2*WINDOWSIZE 
static CnetTimerID  timers[WINDOWSIZE];
//static bool arrived[WINDOWSIZE];
// Keep a list of checksums that we have seen recently.
static int sentSeqNums[MAXSEQ] = {0}; // Will store the  sequence numbers sent.
static int expectedSeqNums[MAXSEQ] = {0};// Will store the expected sequence numbers.
static uint32_t seen_checksums[PACKET_MEMORY_LENGTH];

void sendPri(int);
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
	fprintf(stdout, "Node %d has sent a package to node %d \n",nodeinfo.address, packet.dest);  
	dll_wifi_write(dll_states[i], wifi_dest, (char *)&packet, packet_length);
    }
  }
  //packetSent[packet.dest]= packet;  ///???

}

/// This function will handle our frame timeouts.
///
EVENT_HANDLER(timeouts) {

  struct nl_packet packet = lastpacket;
 
  CnetNICaddr wifi_dest;
  CHECK(CNET_parse_nicaddr(wifi_dest, "ff:ff:ff:ff:ff:ff"));
  
  for (int i = 1; i <= nodeinfo.nlinks; ++i) {
    if (dll_states[i] != NULL) {
      //dll_wifi_write(dll_states[i], wifi_dest, (char *)&packet, packet.length);
    }
  }
  printf("\t\t\t\t\t\tTime out DATA re-transmitted, seq=%d\n",packet.seqNum);
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

/// Called when we encounter a collision.
///
static EVENT_HANDLER(collision) {
  wifi_coll_exp_backoff(dll_states[1].data.wifi); // Call our wifi backoff because mobiles only use the wifi layer.
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

  if (packet.dest == nodeinfo.address) 
  {
	fprintf(stdout, "I GOT A MESSAGE");
	fprintf(stdout, "from %d\n", packet.src);//I had packet.dest here. I was so confused :(
	fprintf(stdout, "\t I am %d btws\n", nodeinfo.address);
  }


  printf("Mobile: Received frame from dll on link %d from node %" PRId32   ////this was dodgy SIGNAL 11 !!!!!!
         " for node %" PRId32 ".\n", link, packet.src, packet.dest);
     
  printf("Mobile: type %d seqNum %d \n", packet.type, packet.seqNum);
  printf("Mobile data: %s\n", data);
  
  // Hold our checksum.
  uint32_t checksum = packet.checksum;
  packet.checksum = 0;
  
  // Set our broadcast.
  CnetNICaddr wifi_dest;
  CHECK(CNET_parse_nicaddr(wifi_dest, "ff:ff:ff:ff:ff:ff"));
     
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
       // sendNext();
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
   
  // Ensure checksum is valid.
  if(CNET_crc32((unsigned char *)&packet, sizeof(packet)) != checksum ) {
	printf("\tChecksum failed  for packet type %d \n", packet.type);
    // Put something to send NACK here.

	printf( "DB::: strcmp: %d\n", 0 == strcmp((char *)&packet.type,"DATA"));
	if (strcmp((char *)&packet.type,"DATA")){
   	 	struct nl_packet npacket = (struct nl_packet) {
    	  		.src = nodeinfo.address,
    	  		.dest = packet.src,
      	  		.length = 0,
     	  		.type = NACK,
    	  		.seqNum = packet.seqNum 
   	 	};     
  	   //uint16_t packet_length = NL_PACKET_LENGTH(npacket);
	   npacket.checksum = CNET_crc32((unsigned char *)&npacket, sizeof(npacket));
   	   // dll_wifi_write(dll_states[link], wifi_dest, (char *)&npacket, packet_length);
	   packetsSent[npacket.dest]= npacket;		
	   sendPri(npacket.dest);
   	   printf("NACK transmitted, seq=%d \n",npacket.seqNum);
	}
    return;
  } 
  
    printf("ELSE");
    switch(packet.type) {
      case ACK: {
    	printf("\t seqNum received:%d expected: %d \n", packet.seqNum, expectedSeqNums[packet.src]);
       // if(packet.seqNum == expectedSeqNums[packet.src]) {
          printf("\t\t\t\tACK received, seq=%d from node %d \n", packet.seqNum, packet.src );
          CNET_stop_timer(timers[packet.src]);
		CNET_enable_application(packet.src);
          //ackexpected = 1-ackexpected;
          //CNET_enable_application(ALLNODES);
       // }
        break;
      }
      case NACK: {
       // if(packet.seqNum == expectedSeqNums[packet.src]) {
          printf("\t\t\t\tNACK received, seq=%d\n", packet.seqNum);
          CNET_stop_timer(timers[packet.src]);
          printf("timeout, seq=%d\n", ackexpected);
	  struct nl_packet rpacket = packetsSent[packet.src];
          ackexpected = 1;     
          //uint16_t packet_length = NL_PACKET_LENGTH(rpacket);
	  rpacket.checksum = CNET_crc32((unsigned char *)&rpacket, sizeof(rpacket));
          //dll_wifi_write(dll_states[link], wifi_dest, (char *)&rpacket, packet_length);
	  sendPri(packet.src);
          printf("DATA re-transmitted, seq=%d\n",rpacket.seqNum);
        //}
        break;
      }
      case DATA: {
        printf("\t\t\t\tDATA received, seq=%d, \n", packet.seqNum);
        if(packet.seqNum == expectedSeqNums[packet.src]) {
          printf("up to application\n");
          //CHECK(CNET_write_application(packet.data, &payload_length));
          expectedSeqNums[packet.src]= 1-expectedSeqNums[packet.src];

		 struct nl_packet apacket = (struct nl_packet){
         		 .src = nodeinfo.address,
          		.dest = packet.src,
          		.length = 0,
          		.type = ACK,
          		.seqNum = packet.seqNum 
       		 };
         
       		 //uint16_t packet_length = NL_PACKET_LENGTH(apacket);
		packet.checksum = CNET_crc32((unsigned char *)&apacket, sizeof(apacket));
       		// dll_wifi_write(dll_states[link], wifi_dest, (char *)&apacket, packet_length);
		packetsSent[apacket.dest]= apacket;
		sendPri(apacket.dest);
        	printf("ACK transmitted, seq=%d\n",apacket.seqNum);

  		size_t payload_length = packet.length;
  		if (payload_length == 0) {printf("got zero length payload.\n");}
  		else	{ // Send this packet to the application layer.
			printf("\t\t\t\t\t&payload_length: %d packet.data:%d \n", packet.data, &payload_length);
    			CHECK(CNET_write_application(packet.data, &payload_length));
 			printf("\tUp to the application layer!\n");
       			 }
		}	
        else {printf("ignored. packet seqNum: %d  expected: %d \n",packet.seqNum, expectedSeqNums[packet.src]);}
              
       
	}
        break;
      }
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
    //.seqNum = 1-nextSeqNum /// global next seqNum
  };

  // Create a RTS packet.
 // struct nl_packet rts = (struct nl_packet) {
  //  .src = nodeinfo.address,
 //   .length = NL_MAXDATA
 // }; 
  
  CHECK(CNET_read_application(&packet.dest, packet.data, &packet.length));
 
  //strcpy(rts.data, "RTS");
  //fprintf(stdout, "node %d %s:\n", nodeinfo. address, rts.data);

  // Create checksum for RTS
 // rts.checksum = CNET_crc32((unsigned char*)&rts, sizeof(rts));
  packet.checksum = CNET_crc32((unsigned char *)&packet, sizeof(packet));
  
  fprintf(stdout, "Mobile: Generated message for %" PRId32
         ", broadcasting on all data link layers\n",
         packet.dest);
  
 
  //lastpacket = packet;
    
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
  timeout = (packet_length*800000000 / linkinfo[1].bandwidth) + /// fix this to expected average
  	linkinfo[1].propagationdelay;
  timers[packet.dest]= CNET_start_timer(EV_TIMER3, timeout, 0);
  
  for (int i = 1; i <= nodeinfo.nlinks; ++i) {
    if (dll_states[i] != NULL) {
	fprintf(stdout, "Node %d has RTS\n",nodeinfo.address);                                                 
        //dll_wifi_write(dll_states[i], wifi_dest, (char *)&rts, packet_length); 

	MESSAGE m = {
		.dest = packet.dest,
		.data = packet.data
	};

	buffer[last] = m;
	last ++;

	dll_wifi_write(dll_states[i], wifi_dest, (char *)&packet, packet_length);
    }
  }
 assert(packet.dest < WINDOWSIZE);
sentSeqNums[packet.dest]= 1- sentSeqNums[packet.dest];
  packet.seqNum = sentSeqNums[packet.dest];
 sendPri(packet.dest);
CNET_disable_application(packet.dest);
    printf("DATA transmitted, seq=%d\n",packet.seqNum);
	//ackexpected = 1;

packetsSent[packet.dest] = packet;
}

    void sendPri(int num)
    {
    // Create a RTS packet.
    struct nl_packet rts = (struct nl_packet) {
    .src = nodeinfo.address,
    .length = NL_MAXDATA
    };
    //CHECK(CNET_read_application(&packet.dest, packet.data, &packet.length));
    sprintf(rts.data, "%s%d", "RTS_PRI", num);                                 /////// this was dodgy SIGNAL 11!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    // Create checksum for RTS
   // rts.checksum = CNET_crc32((unsigned char*)&rts, sizeof(rts));
    // Create a broadcast address.
   // CnetNICaddr wifi_dest;
    //CHECK(CNET_parse_nicaddr(wifi_dest, "ff:ff:ff:ff:ff:ff"));
    //CnetNICaddr rts_dest;
    //CHECK(CNET_parse_nicaddr(rts_dest, "ff:ff:ff:ff:ff:ff"));
   // uint16_t packet_length = NL_PACKET_LENGTH(rts);
    //for (int i = 1; i <= nodeinfo.nlinks; ++i) {
   // if (dll_states[i] != NULL) {
    //fprintf(stdout, "Node %d has RTS_PRI %d\n", nodeinfo.address, num);
   // dll_wifi_write(dll_states[i], wifi_dest, (char *)&rts, packet_length);
    //}
   // }
    }
/// Called when this mobile node is booted up.
///
void reboot_mobile() {
  // We require each node to have a different stream of random numbers.
  CNET_srand(nodeinfo.time_of_day.sec + nodeinfo.nodenumber);

  // Provide the required event handlers.
  CHECK(CNET_set_handler(EV_PHYSICALREADY, physical_ready, 0));
  CHECK(CNET_set_handler(EV_APPLICATIONREADY, application_ready, 0));
  CHECK(CNET_set_handler(EV_FRAMECOLLISION, collision, 0));
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
