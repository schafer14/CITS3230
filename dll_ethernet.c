/// This file implements our Ethernet data link layer.

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

/// This struct type will hold the state for one instance of the Ethernet data
/// link layer. The definition of the type is not important for clients.
///
struct dll_eth_state {
  // The link that this instance of the Ethernet protocol is associated with.
  int link;
  
  // A pointer to the function that is called to pass data up to the next layer.
  up_from_dll_fn_ty nl_callback;

  // Add members to represent the Ethernet link's state here.
};

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

CnetTimerID lasttimer = NULLTIMER;	// Create a cnet timer to hold our carrier sense data.
CnetTimerID lasttimer4 = NULLTIMER;	// Create a cnet timer to hold our collision data.
struct eth_frame tempframe;		// This will hold the temporary frame that has to be delayed when the line is busy.
struct eth_frame collframe;		// This will hold the temporary frame in case of collision.
struct dll_eth_state *tempstate;	// Take note of the state of the ethernet link when the line is busy.
struct dll_eth_state *collstate;	// Take note of the state of the ethernet link in case of collision.
size_t tempframe_length;		// Hold the frame length for retransmission use.
size_t collframe_length;		// Hold the frame lenfth in case of collision.
int collisions;				// Number of collisions.

#define ETH_HEADER_LENGTH (offsetof(struct eth_frame, data))

/// If line is busy try and retransmit.
///
static EVENT_HANDLER(IFG_timeout) {
  dll_eth_write(tempstate, tempframe.dest, tempframe.data, tempframe_length);	// Try and retransmit our frame.
}

/// If there is a collision try and retransmit.
///
static EVENT_HANDLER(coll_timeout) {
  fprintf(stdout, "node:%d, collision, framesize is %d\n", nodeinfo.address, (int)(collframe_length+ETH_HEADER_LENGTH));
  collisions++;
  dll_eth_write(collstate, collframe.dest, collframe.data, collframe_length);	// Try and retransmit our frame.
}

/// This function will calculate our exponential backoff time.
///
void eth_exp_backoff() {
  srand(time(NULL)); // Create a new seed to be used in our rand function.
  int c;        // Create an integer to be used to help generate our random backoff time.

  // If more than 16 delays discard the frame.
  if(collisions == 16) {
    collisions = 0;
    return;
  }
  else if(collisions >= 10) c = rand() % (int)pow(2, 10);     // Back off for maximum time.
  else if(collisions < 10) c = rand() % (int)pow(2, collisions);    // Calculate backoff time.

  CnetTime backoff = ((CnetTime)SLOT * c);      // Set amount of time to delay.
  lasttimer4 = CNET_start_timer(EV_TIMER4, backoff, 0); // Start timer.
  return;
}

/// If there is a collision backoff for random period and try and retransmit.
///
static EVENT_HANDLER(collision) {
  eth_exp_backoff();
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
  
  // Call our required handlers
  CHECK(CNET_set_handler(EV_TIMER1, IFG_timeout, 0));
  CHECK(CNET_set_handler(EV_TIMER4, coll_timeout, 0));  

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

    tempstate = state;	// Assign a pointer that points to our current state.

    //	Fill our tempframe with buffered details to be retransmitted.
    memcpy(tempframe.dest, dest, sizeof(CnetNICaddr));
    memcpy(tempframe.src, linkinfo[state->link].nicaddr, sizeof(CnetNICaddr));
    memcpy(tempframe.type, &length, sizeof(length));
    memcpy(tempframe.data, data, length);
    tempframe_length = length;
    
    //fprintf(stdout, "eth line busy, waiting...\n");

    CnetTime backoff = ((CnetTime)IFG);	// Backoff for the interframe gap.
    lasttimer = CNET_start_timer(EV_TIMER1, backoff, 0); // Start timer.
    return;
  } 
  collisions = 1;
  if (!data || length == 0) return;	// If data is invalid discard.
  
  collstate = state;	// Assign a pointer to our current state in case of collision.
  
  struct eth_frame frame;	// Create a frame
    
  // Set the destination and source address, store data in case of collision as well.
  memcpy(frame.dest, dest, sizeof(CnetNICaddr));
  memcpy(frame.src, linkinfo[state->link].nicaddr, sizeof(CnetNICaddr));
  memcpy(collframe.dest, dest, sizeof(CnetNICaddr));
  memcpy(collframe.src, linkinfo[state->link].nicaddr, sizeof(CnetNICaddr));
      
  // Set the length of the payload.
  memcpy(frame.type, &length, sizeof(length));
  memcpy(collframe.type, &length, sizeof(length));
    
  // Copy the payload into the frame, store data in case of collision as well.
  memcpy(frame.data, data, length);
  memcpy(collframe.data, data, length);
  collframe_length = length;
      
  // Calculate the number of bytes to send.
  size_t frame_length = length + ETH_HEADER_LENGTH;
  if (frame_length < ETH_MINFRAME) frame_length = ETH_MINFRAME;	// If frame length is less than the minimum frame size pad the frame to the minimum size.

  CHECK(CNET_write_physical(state->link, &frame, &frame_length));	// Write the frame to the physical layer.
  CHECK(CNET_set_handler(EV_FRAMECOLLISION, collision, 0));
  fprintf(stdout, "node:%d eth line success, framesize is %d\n", nodeinfo.address, (int)frame_length);
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
