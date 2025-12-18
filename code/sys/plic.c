// plic.c - RISC-V PLIC
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef PLIC_TRACE
#define TRACE
#endif

#ifdef PLIC_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "plic.h"
#include "assert.h"

#include <stdint.h>

// INTERNAL MACRO DEFINITIONS
//

// CTX(i,0) is hartid /i/ M-mode context
// CTX(i,1) is hartid /i/ S-mode context

#define CTX(i,s) (2*(i)+(s))

// INTERNAL TYPE DEFINITIONS
// 

struct plic_regs {
	union {
		uint32_t priority[PLIC_SRC_CNT];
		char _reserved_priority[0x1000];
	};

	union {
		uint32_t pending[PLIC_SRC_CNT/32];
		char _reserved_pending[0x1000];
	};

	union {
		uint32_t enable[PLIC_CTX_CNT][32];
		char _reserved_enable[0x200000-0x2000];
	};

	struct {
		union {
			struct {
				uint32_t threshold;
				uint32_t claim;
			};
			
			char _reserved_ctxctl[0x1000];
		};
	} ctx[PLIC_CTX_CNT];
};

#define PLIC (*(volatile struct plic_regs*)PLIC_MMIO_BASE)

// INTERNAL FUNCTION DECLARATIONS
//

static void plic_set_source_priority (
	uint_fast32_t srcno, uint_fast32_t level);
static int plic_source_pending(uint_fast32_t srcno);
static void plic_enable_source_for_context (
	uint_fast32_t ctxno, uint_fast32_t srcno);
static void plic_disable_source_for_context (
	uint_fast32_t ctxno, uint_fast32_t srcno);
static void plic_set_context_threshold (
	uint_fast32_t ctxno, uint_fast32_t level);
static uint_fast32_t plic_claim_context_interrupt (
	uint_fast32_t ctxno);
static void plic_complete_context_interrupt (
	uint_fast32_t ctxno, uint_fast32_t srcno);

static void plic_enable_all_sources_for_context(uint_fast32_t ctxno);
static void plic_disable_all_sources_for_context(uint_fast32_t ctxno);

// We currently only support single-hart operation, sending interrupts to S mode
// on hart 0 (context 0). The low-level PLIC functions already understand
// contexts, so we only need to modify the high-level functions (plit_init,
// plic_claim_request, plic_finish_request)to add support for multiple harts.

// EXPORTED FUNCTION DEFINITIONS
// 

void plic_init(void) {
	int i;

	// Disable all sources by setting priority to 0

	for (i = 0; i < PLIC_SRC_CNT; i++)
		plic_set_source_priority(i, 0);
	
	// Route all sources to S mode on hart 0 only

	for (int i = 0; i < PLIC_CTX_CNT; i++)
		plic_disable_all_sources_for_context(i);
	
	plic_enable_all_sources_for_context(CTX(0,1));
}

extern void plic_enable_source(int srcno, int prio) {
	trace("%s(srcno=%d,prio=%d)", __func__, srcno, prio);
	assert (0 < srcno && srcno <= PLIC_SRC_CNT);
	assert (prio > 0);

	plic_set_source_priority(srcno, prio);
}

extern void plic_disable_source(int irqno) {
	if (0 < irqno)
		plic_set_source_priority(irqno, 0);
	else
		debug("plic_disable_irq called with irqno = %d", irqno);
}

extern int plic_claim_interrupt(void) {
	// FIXME: Hardwired S-mode hart 0
	
	trace("%s()", __func__);
	return plic_claim_context_interrupt(CTX(0,1));
}

extern void plic_finish_interrupt(int irqno) {
	// FIXME: Hardwired S-mode hart 0
	
	trace("%s(irqno=%d)", __func__, irqno);
	plic_complete_context_interrupt(CTX(0,1), irqno);
}

// INTERNAL FUNCTION DEFINITIONS
//

// static inline void plic_set_source_priority(uint_fast32_t srcno, uint_fast32_t level)
// Inputs: uint_fast32_t srcno - interrupt source number, uint_fast32_t level - priority level
// Outputs: None
// Description: This function sets the priority level of an interrupt source. It will change the priority array based of the interrupt source number.
// Side Effects: Changes the priority array

static inline void plic_set_source_priority(uint_fast32_t srcno, uint_fast32_t level) {
	// FIXME your code goes here
	PLIC.priority[srcno] = level; // sets the priority level in the priority array at the index of the source number
}


// static inline int plic_source_pending(uint_fast32_t srcno)
// Inputs: uint_fast32_t srcno - interrupt source number
// Outputs: Returns 1 if there is a pending interrupt, otherwise it returns 0 if there isn't
// Description: This function checks if an interrupt source is pending. It will check the bit in the pending array that corresponds to the source number and sees if the pending bit is 1 or 0.
// Side Effects: None

static inline int plic_source_pending(uint_fast32_t srcno) {
	// FIXME your code goes here
	uint_fast32_t bit = 1;
	uint_fast32_t word = srcno / 32; // gets the index of the 32 bits that you want to see and also the bit that you want to check
	bit = bit << (srcno % 32);
	if((PLIC.pending[word] & bit) != 0){ // Ands the word of 32 bits with the bit that you want to see in the pending array to see if there is an interrupt pending or not
		return 1;
	}
	else{
		return 0;
	}
}

// static inline void plic_enable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcno)
// Inputs: uint_fast32_t ctxno - context number, uint_fast32_t srcno - interrupt source number
// Outputs: None
// Description: This function enables the interrupt source for the context. It will set the correct bit in the enable array and calulates the correct index based of the source number and the context number. Then it will set the correct bit.
// Side Effects: Changes the enable array

static inline void plic_enable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcno) {
	// FIXME your code goes here
	uint_fast32_t bit = 1;
	uint_fast32_t word = srcno / 32; // gets the index of the 32 bits that you want to see and also the bit that you want to check
	bit = bit << (srcno % 32);
	PLIC.enable[ctxno][word] |= bit; // sets the bit based the index of the context number and the word and enables the interrupt of the correct bit 
}


// static inline void plic_disable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcid)
// Inputs: uint_fast32_t ctxno - context number, uint_fast32_t srcid - interrupt source number
// Outputs: None
// Description: This function disables the interrupt source for the context. It will clear the correct bit in the enable array and calulates the correct index based of the source number and the context number. Then it will clear the correct bit.
// Side Effects: Changes the enable array

static inline void plic_disable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcid) {
	// FIXME your code goes here
	uint_fast32_t bit = 1;
	uint_fast32_t word = srcid / 32; // gets the index of the 32 bits that you want to see and also the bit that you want to check
	bit = ~(bit << (srcid % 32));
	PLIC.enable[ctxno][word] &= bit; // clears the bit based the index of the context number and the word and disables the interrupt of the correct bit 
}

// static inline void plic_set_context_threshold(uint_fast32_t ctxno, uint_fast32_t level)
// Inputs: uint_fast32_t ctxno - context number, uint_fast32_t level - priority level
// Outputs: None
// Description: This function sets the interrupt priority threshold for a context. It sets a thresholf and the context should ignore any interrupts below the priority.
// Side Effects: Changes the context array and threshold register

static inline void plic_set_context_threshold(uint_fast32_t ctxno, uint_fast32_t level) {
	// FIXME your code goes here
	PLIC.ctx[ctxno].threshold = level; // sets the priority level in the threshold register in the context array at the index of the context number
}

// static inline uint_fast32_t plic_claim_context_interrupt(uint_fast32_t ctxno)
// Inputs: uint_fast32_t ctxno - context number
// Outputs: Returns the interrupt source number, otherwise returns 0 if there are no interrupts pending
// Description: This function claims the interrupt for a given context. It will read from the claim register and return the interrupt ID of the highest priority pending interrupt
// Side Effects: Changes the pending array

static inline uint_fast32_t plic_claim_context_interrupt(uint_fast32_t ctxno) {
	// FIXME your code goes here
	uint_fast32_t scrno = PLIC.ctx[ctxno].claim; //gets the interrupt ID from the claim register in the context array based of the index of the context number
	if(scrno > PLIC.ctx[ctxno].threshold){
		uint_fast32_t word = scrno / 32; // gets the index of the 32 bits that you want to see and also the bit that you want to check
		uint_fast32_t bit = 1;
		bit = ~(bit << (scrno % 32));
		PLIC.pending[word] &= bit; // clears the bit based the index of the word in the pending array and disables the interrupt of the correct bit 
		return scrno;
	}
	return scrno;
}

// static inline void plic_complete_context_interrupt(uint_fast32_t ctxno, uint_fast32_t srcno)
// Inputs: uint_fast32_t ctxno - context number, uint_fast32_t srcno - interrupt source number
// Outputs: None
// Description: This function completes the handling of an interrupt for a given context. It will write the interrupt source number back to the claim register and notify the PLIC that the interrupt is serviced.
// Side Effects: Changes the context array and claim register

static inline void plic_complete_context_interrupt(uint_fast32_t ctxno, uint_fast32_t srcno) {
	// FIXME your code goes here
	PLIC.ctx[ctxno].claim = srcno; //sets the interrupt ID from the claim register in the context array based of the index of the context number
}

// static void plic_enable_all_sources_for_context(uint_fast32_t ctxno)
// Inputs: uint_fast32_t ctxno - context number
// Outputs: None
// Description: This function enable all interrupt sources for a given context. It will set all of the bits in the corresponding entry of the entry of the enable array based of the specified context.
// Side Effects: Changes the enable array

static void plic_enable_all_sources_for_context(uint_fast32_t ctxno) {
	// FIXME your code goes here
	for(int i = 0; i < PLIC_SRC_CNT; i++){
		plic_enable_source_for_context(ctxno, i); // goes through the entire context and sets all of the bits, which enables all of the interrupts
	}
}

// static void plic_disable_all_sources_for_context(uint_fast32_t ctxno)
// Inputs: uint_fast32_t ctxno - context number
// Outputs: None
// Description: This function disable all interrupt sources for a given context. It will clear all of the bits in the corresponding entry of the entry of the enable array based of the specified context.
// Side Effects: Changes the enable array

static void plic_disable_all_sources_for_context(uint_fast32_t ctxno) {
	// FIXME your code goes here
	for(int i = 0; i < PLIC_SRC_CNT; i++){
		plic_disable_source_for_context(ctxno, i); // goes through the entire context and clears all of the bits, which disables all of the interrupts
	}
}