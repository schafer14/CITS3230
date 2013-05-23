/// This file implements our Ethernet data link layer. We added carrier sense to this data link layer but
//  we could not get the collision retransmission to work properly so at the moment we sense collisions but do not retransmit.

#include "dll_ethernet.h"

#include <cnet.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <math.h>

#define ETH_MAXDATA 1500	// The maximum amount of data we can send.
#define ETH_MINFRAME 64		// The minimum amount of data we can send.
#define IFG 9.6			// Our interframe gap period that a link will wait before retransmitting if the line is busy.
#define SLOT 51.2		// Our slot time to be used for exponential backoff in the event of a collision.		

/// This struct specifies the format of an Ethernet frame. When using Ethernet
/// links in cnet, the first part of the frame must be the destination address.
///
struct eth_frame {
  // Ethernet address of the destination (receiver).
  CnetNICaddr dest;

  // Ethernet address of the source (sender).
  CnetNICaddr src;

  // For our protocol the type field will indicate the length of the payload.
  char type[2];

  // Data must be the last field, because we will truncate the unused area when
  // sending to the physical layer.
  char data[ETH_MAXDATA];
};

/// This struct type will hold the state for one instance of the Ethernet data
/// link layer. The definition of the type is not important for clients.
///
struct dll_eth_state {
  // The link that this instance of the Ethernet protocol is associated with.
  int link;
  
  // A pointer to the function that is called to pass data up to the next layer.
  up_from_dll_fn_ty nl_callback;
 
  // This will store a version of the frame incase of collision.
  //struct eth_frame collframe;	// Does not work.

  // This will store the size of our frame incase of collision.
  //uint16_t collframe_length; // Does not work.
 
  // This will count the number of collisions.
  //int collisions; // Does not work.
 
  // Add members to represent the Ethernet link's state here.
};

CnetTimerID lasttimer = NULLTIMER;	// Create a cnet timer to hold our carrier sense data.
//CnetTimerID lasttimer4 = NULLTIMER;	// Create a cnet timer to use during our collisions. Doesnt work.
struct eth_frame tempframe;		// This will hold the temporary frame that has to be delayed when the line is busy.
uint16_t tempframe_length;		// Hold the frame length for retransmission use.

#define ETH_HEADER_LENGTH (offsetof(struct eth_frame, data))

/// This will be called when our frame collides
///
/*static EVENT_HANDLER(coll_timeout) {
  struct dll_eth_state *state = (struct dll_eth_state *)data;
  dll_eth_write(state, state->collframe.dest, state->collframe.data, state->collframe_length); 
}*/	// Does not work.
 
/// If line is busy try and retransmit.
///
static EVENT_HANDLER(IFG_timeout) {
  dll_eth_write((struct dll_eth_state *)data, tempframe.dest, tempframe.data, tempframe_length);	// Try and retransmit our frame.
}

/// This function will process our exponential backoff when a collision occurs.
///
void eth_coll_exp_backoff(struct dll_eth_state *state) {
  //state->collisions++;
  printf("ETH: collision. waiting....\n");
  //srand(time(NULL)); // Create a new seed to be used in our rand function.
  //int c;        // Create an integer to be used to help generate our random backoff time.

  // If more than 16 delays ethernet layer is assumed severed and we exit.
  /*if(collisions > 16) {
    fprintf(stderr, "ERROR: too many collisions\n");
    exit(EXIT_FAILURE);
  }
  else if(collisions >= 10) c = rand() % (int)pow(2, 10);      // Back off for maximum time.
  else if(collisions < 10) c = rand() % (int)pow(2, collisions);      // Calculate backoff time.
  CnetTime backoff = ((CnetTime)SLOT * c);      // Set amount of time to delay.
  lasttimer4 = CNET_start_timer(EV_TIMER4, backoff, (CnetData)state);   // Start timer.
  */	// Does not work.
  return;
}

/// Create a new state for an instance of the Ethernet data link layer.
///
struct dll_eth_state *dll_eth_new_state(int link, up_from_dll_fn_ty callback) {
  // Ensure that the given link exists and is a LAN link.
  if (link > nodeinfo.nlinks || linkinfo[link].linktype != LT_LAN)
    return NULL;
  
  // Allocate memory for the state.
  struct dll_eth_state *state = calloc(1, sizeof(struct dll_eth_state));
  
  // Check whether or not the allocation was successful.
  if (state == NULL)
    return NULL;
  
  // Initialize the members of the structure.
  state->link = link;
  state->nl_callback = callback;
  //state->collisions = 0;  // Does not work.
    
  // Call our required handlers
  CHECK(CNET_set_handler(EV_TIMER1, IFG_timeout, 0));
  //CHECK(CNET_set_handler(EV_TIMER4, coll_timeout, 0));	// Does not work.

  return state;
}

/// Delete the given dll_eth_state. The given state pointer will be invalid
/// following a call to this function.
///
void dll_eth_delete_state(struct dll_eth_state *state) {
  if (state == NULL) return;	// If state is already empty then return.
  
  // Free any dynamic memory that is used by the members of the state.
  free(state);
}

/// Write a frame to the given Ethernet link.
///
void dll_eth_write(struct dll_eth_state *state,
                   CnetNICaddr dest,
                   const char *data,
                   uint16_t length) {
  // If line is transmitting
  if(CNET_carrier_sense(state->link) == 1) {
    if (!data || length == 0) return;	// If data is invalid discard.

    // Fill our tempframe with buffered details to be retransmitted.
    memcpy(tempframe.dest, dest, sizeof(CnetNICaddr));
    memcpy(tempframe.data, data, length);
    tempframe_length = length;
    
    CnetTime backoff = ((CnetTime)IFG);	// Backoff for the interframe gap.
    lasttimer = CNET_start_timer(EV_TIMER1, backoff, (CnetData)state); // Start timer.

    printf("ETH: line busy, waiting....\n");
    return;
  }
  if (!data || length == 0) return;	// If data is invalid discard.
  
  struct eth_frame frame;                 // This will hold our data for transmission.
  
  // Set the destination and source address
  memcpy(frame.dest, dest, sizeof(CnetNICaddr));
  memcpy(frame.src, linkinfo[state->link].nicaddr, sizeof(CnetNICaddr));
      
  // Set the length of the payload.
  memcpy(frame.type, &length, sizeof(length));
    
  // Copy the payload into the frame.
  memcpy(frame.data, data, length);
  
  // Copy the frame and frame length across to be used in the event of a collision.
  // state->collframe = iframe;	// Does not work.
  // state->collframe_length = length;	// Does not work.
    
  // Calculate the number of bytes to send.
  size_t frame_length = length + ETH_HEADER_LENGTH;
  if (frame_length < ETH_MINFRAME) frame_length = ETH_MINFRAME;	// If frame length is less than the minimum frame size pad the frame to the minimum size.

  CHECK(CNET_write_physical(state->link, &frame, &frame_length));	// Write the frame to the physical layer.
}

/// Called when a frame has been received on the Ethernet link. This function
/// will retrieve the payload, and then pass it to the callback function that
/// is associated with the given state struct.
///
void dll_eth_read(struct dll_eth_state *state,
                  const char *data,
                  size_t length) {
  // If length is larger than our frame discard frame.
  if (length > sizeof(struct eth_frame)) {
    return;
  }
  
  // Treat the data as an Ethernet frame.
  const struct eth_frame *frame = (const struct eth_frame *)data;
  
  // Extract the length of the payload from the Ethernet frame.
  uint16_t payload_length = 0;
  memcpy(&payload_length, frame->type, sizeof(payload_length));
  
  // Send the frame up to the next layer.
  if (state->nl_callback)
    (*(state->nl_callback))(state->link, frame->data, payload_length);
} 
