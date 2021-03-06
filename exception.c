/*
Copyright 2013, Jernej Kovacic

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

   http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/


/**
 * @file
 *
 * Implementation of ARM exception handlers (except the reset handler which is 
 * implemented in vector.s). Implementation of handlers inside C functions
 * is handy as attributes ("__attribute__((interrupt))") take care of
 * handlers' necessary "boiler plate code".
 * 
 * The most important handlers are SWI and IRQ interrupt handlers.
 * 
 * @author Jernej Kovacic
 */

#include <stdint.h>

/* Starting address of the memory where interrupt vectors are actually expected: */
#define MEM_DST_START       0x00000000

#define MAX_ADDRESS         UINT32_MAX


/* Declaration of IRQ handler routine, implemented in interrupt.c */
extern void _pic_IrqHandler(void);

/*
 * Whenever an IRQ interrupt is triggered, this exception handler is called
 * that further calls the IRQ handler routine. The routine is implemented
 * in interrupt.c. 
 */
void __attribute__((interrupt("IRQ"))) irq_handler(void) 
{
    _pic_IrqHandler();
}


/*
 * Whenver a SWI (or its equivalent SVC) instruction is called, the CPU
 * switches into the Supervisor mode and executes this handler. It is
 * particularly handy when a privileged operation (e.g.modification of CSPR
 * register's bits) is required from an unprivileged mode (e.g. User).
 * 
 * This implementation is very trivial. It checks the immediate value,
 * "appended" to the SWI instruction. If it equals 0, caller mode's CSPR's 
 * I bit will be set (i.e. IRQ handler will be disabled), otherwise it will
 * be cleared.
 */
void __attribute__((interrupt("SWI"))) swi_handler(void) 
{
    /*
     * CSPR ans SPSR can only be accessed via assembler.
     */
    
    /* Extract the immediate value (lower 24 bits of the actual instruction ): */
    __asm volatile("LDR r0, [lr, #-4]");         /* load the actual SWI instruction */
    __asm volatile("BIC r0, r0, #0xFF000000");   /* clear the highest 8 bits */
    
    __asm volatile("MRS r1, spsr");              /* load the SPSR into r1 */
    
    __asm volatile("TST r0, #0xFF");             /* test whether the immed. value equals 0 */
    __asm volatile("ORREQ r1, r1, #0x80");       /* if yes, set the I bit */
    __asm volatile("BICNE r1, r1, #0x80");       /* otherwise clear the I bit */
    __asm volatile("MSR spsr_cxsf, r1");         /* finally update the SPSR */
    
    /* TODO add support for Thumb mode (16 bit) */
}



/*
 * All other exception handlers are implemented as infinite loops. 
 */

void __attribute__((interrupt("FIQ"))) fiq_handler(void) 
{ 
    for( ; ; ); 
}

void __attribute__((interrupt("UNDEF"))) undef_handler(void) 
{ 
    for( ; ; ); 
}

void __attribute__((interrupt("ABORT"))) prefetch_abort_handler(void)
{ 
    for( ; ; ); 
}

void __attribute__((interrupt("ABORT"))) data_abort_handler(void)
{ 
    for( ; ; ); 
}


/*
 * 'vectors_start' and 'vectors_end' with exception handling vectors are placed
 * to point where the code is actually loaded (0x00010000 by Qemu), they must be copied
 * to the place where the ARM CPU actually expects them, i.e. at the begining of the
 * memory (0x00000000)
 */
void copy_vectors(void) 
{
    /* Both labels are declared and appropriately positioned in vectors.s: */
    extern uint32_t vectors_start;
    extern uint32_t vectors_end;
    
    /*
     * Handle a very unlikely case that 'vectors_start' is located after 'vectors_end'
     */
    uint32_t* const src_begin = ( &vectors_start<=&vectors_end ? &vectors_start : &vectors_end );
    uint32_t* const src_end = ( &vectors_end>=&vectors_start ? &vectors_end : &vectors_start );
    const uint32_t block_len = src_end - src_begin;
    
    uint32_t* vectors_src;
    uint32_t* vectors_dst;
    uint32_t* const dst_start = (uint32_t* const) MEM_DST_START;
 
    
    /* 
     * No need to copy anything if '&vectors_start' equals 'dst_start' 
     * This prevents potential problems if attempting to "copy" from
     * 0x00000000 to 0x00000000.
     */
    if ( dst_start == src_begin )
    {
        return;
    }
    
    /* Nothing is done if forward copy would exceed the addressable range: */
    if ( block_len > ((uint32_t* const) MAX_ADDRESS - dst_start) )
    {
        return;
    }


    if ( dst_start < src_begin || dst_start >= src_end )
    {
        /*
         * If vectors are copy backwards or if destination block starts after
         * 'vectors_end', it is completely safe to copy the source block word by word 
         * from begin to end. Memory corruption due to overlapping of memory areas is
         * not possible.
         */
        vectors_src = src_begin;
        vectors_dst = dst_start;
        
        while (vectors_src < src_end)
        {
            *vectors_dst++ = *vectors_src++;
        }
    }
    else
    {
        /*
         * If vectors are copied forward and the destination starts before the source's end,
         * memory corruption is possible if words are copied from the source's start to the end.
         * This is why, vectors will be copied word by word from the source's end towards
         * its start.
         */
        vectors_src =  src_end - 1;
        vectors_dst = dst_start + block_len - 1;
        
        /*
         * Inside this block, 'dst_start' is always greater than 0x00000000.
         * So 'vectors_dst' can never overflow after decrement. As such it is safer
         * to use as a condition variable of the while loop.
         */
        while ( vectors_dst >= dst_start )
        {
            *vectors_dst-- = *vectors_src--;
        }
    }
}
