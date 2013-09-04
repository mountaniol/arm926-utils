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
 * Implementation of the board's Primary Interrupt Controller (PIC) functionality.
 * 
 * Secondary Interrupt Controller (SIC) is currently not supported.
 *
 * More info about the board and the PIC controller:
 * - Versatile Application Baseboard for ARM926EJ-S, HBI 0118 (DUI0225D):
 *   http://infocenter.arm.com/help/topic/com.arm.doc.dui0225d/DUI0225D_versatile_application_baseboard_arm926ej_s_ug.pdf
 * - PrimeCell Vectored Interrupt Controller (PL190) Technical Reference Manual (DDI0181):
 *   http://infocenter.arm.com/help/topic/com.arm.doc.ddi0181e/DDI0181.pdf
 *
 * Useful details about the CPU's IRQ mode are available at:
 * - ARM9EJ-S, Technical Reference Manual (DDI0222):
 *   http://infocenter.arm.com/help/topic/com.arm.doc.ddi0222b/DDI0222.pdf 
 * 
 * @author Jernej Kovacic
 */

#include <stdint.h>
#include <stddef.h>

#include "bsp.h"

/* For public definitions of types: */
#include "interrupt.h"



/*
 * 32-bit registers of the Primary Interrupt Controller,
 * relative to the controller's base address:
 * See page 3-3 of DDI0181.
 * 
 * Although not explicitly mentioned by DDI0181, there are gaps
 * among certain groups of registers. The gaps are filled by
 * Unused* "registers" and are treated as "should not be modified".
 */
typedef struct _ARM926EJS_PIC_REGS
{
    const uint32_t VICIRQSTATUS;      /* IRQ Status Register, read only */
    const uint32_t VICFIQSTATUS;      /* FIQ Status Register, read only */
    const uint32_t VICRAWINTR;        /* Raw Interrupt Status Register, read only */
    uint32_t VICINTSELECT;            /* Interrupt Select Register */
    uint32_t VICINTENABLE;            /* Interrupt Enable Register */
    uint32_t VICINTENCLEAR;           /* Interrupt Enable Clear Register */
    uint32_t VICSOFTINT;              /* Software Interrupt Register */
    uint32_t VICSOFTINTCLEAR;         /* Software Interrupt Clear Register */
    uint32_t VICPROTECTION;           /* Protection Enable Register */
    const uint32_t Unused1[3];        /* Unused, should not be modified*/
    uint32_t VICVECTADDR;             /* Vector Address Register */
    uint32_t VICDEFVECTADDR;          /* Default Vector Address Register */
    const uint32_t Unused2[50];       /* Unused, should not be modified */
    uint32_t VICVECTADDRn[16];        /* Vector Address Registers */
    const uint32_t Unused3[48];       /* Unused, should not be modified */
    uint32_t VICVECTCNTLn[16];        /* Vector Control Registers */
    const uint32_t Unused4[872];      /* Unused, should not be modified */
    const uint32_t VICPERIPHID[4];    /* Peripheral Identification Registers, read only */
    const uint32_t VICCELLID[4];      /* PrimeCell Identification Registers, read only */
} ARM926EJS_PIC_REGS;


/* 
 * Register map of the SIC is provided, but it is commented out
 * until the SIC is actually supported.
 */
#if 0
/*
 * 32-bit Registers of the Secondary Interrupt Controller,
 * relative to the controller's base address:
 * See page 4-49 of DUI0225D
 *
 * Note that the SIC is implemented inside a FPGA and is
 * not equal to the PIC. It triggers interrupt 31 on the PIC.
 * 
 * Although not explicitly mentioned by DUI0225D, there is a gap
 * among registers. The gap is filled by Unused1 and is treated 
 * as "should not be modified".
 *
 * Note that some registers share their addresses. See #defines below.
 */
typedef struct _ARM926EJS_SIC_REGS
{
    const uint32_t SIC_STATUS;        /* Status of interrupt (after mask), read only */
    const uint32_t SIC_RAWSTAT;       /* Status of interrupt (before mask), read only */
    uint32_t SIC_ENABLE;              /* Interrupt mask; also SIC_ENSET */
    uint32_t SIC_ENCLR;               /* Clears bits in interrupt mask */
    uint32_t SIC_SOFTINTSET;          /* Set software interrupt */
    uint32_t SIC_SOFTINTCLR;          /* Clear software interrupt */
    const uint32_t Unused1[2];        /* Unused, should not be modified */
    uint32_t SIC_PICENABLE;           /* Read status of pass-through mask; also SIC_PICENSET */
    uint32_t SIC_PICENCLR;            /* Clear interrupt pass through bits */
} ARM926EJS_SIC_REGS;

/* SIC_ENSET (Set bits in interrupt mask) shares its address with SIC_ENABLE. */
#define SIC_ENSET       SIC_ENABLE
/* SIC_PICENSET (Set interrupt pass through bits) shares its address with SIC_PICENABLE. */
#define SIC_PICENSET    SIC_PICENABLE

#endif /* if 0 */


#define UL1                    0x00000001
#define BM_IRQ_PART            0x0000001F
#define BM_VECT_ENABLE_BIT     0x00000020

#define NR_VECTORS      16
#define NR_INTERRUPTS   32


static volatile ARM926EJS_PIC_REGS* const pPicReg = (ARM926EJS_PIC_REGS*) (BSP_PIC_BASE_ADDRESS);
/* static volatile ARM926EJS_SIC_REGS* const pSicReg = (ARM926EJS_SIC_REGS*) (BSP_SIC_BASE_ADDRESS); */


/*
 * A table with addresses of ISR routines for each IRQ request between 0 and 31,
 * and pointers to routines' parameters if applicable.
 * The table is used for non vectored interrupt handling only.
 */
typedef struct _isrNvRecord
{
    int8_t irq;                      /* IRQ handled by this record */
    pNonVectoredIsrPrototype isr;    /* address of the ISR */
    void* param;                     /* void* casted pointer to isr's paramter (if applicable) */
    int8_t priority;                 /* priority of this IRQ */
} isrNvRecord;

static isrNvRecord __isrNV[NR_INTERRUPTS];


/*
 * A table with IRQs serviced by each VICVECTADDRn.
 * If a table's field is negative, its corresponding VICVECTADDRn presumably
 * does not serve any IRQ. In this case, the corresponding VICVECTCNTLn is
 * supposed to be set ot 0 and its VICVECTADDRn should be set to __irq_dummyISR.
 */
typedef struct _isrVectRecord
{
    int8_t irq;                   /* IRQ handled by this record */
    pVectoredIsrPrototype isr;    /* address of the ISR */
    int8_t priority;              /* priority of this IRQ */
} isrVectRecord;

static isrVectRecord __irqVect[NR_INTERRUPTS];


/*
 * IRQ handling mode:
 * - 0: nonvectored mode
 * - any other value: vectored mode
 */ 
static volatile int8_t __irq_vector_mode = 0;


/*
 * A "hidden" function that determines IRQ handling policy (vectored vs nonvectored).
 * It is used in this testing application for testing purposes only, in real world applications 
 * it should never be used, this is why it should not be exposed in a .h file.
 * 
 * @param mode - 0 for non-vectored IRQ handling, any other value for vectored IRQ handling
 */
void _pic_set_irq_vector_mode(int8_t mode)
{
    /* just set a variable to the requested mode */
    __irq_vector_mode = mode;
}


/**
 * Enable CPU's IRQ mode that handles IRQ interrupr requests.
 */
void irq_enableIrqMode(void) 
{
    /*
     * To enable IRQ mode, bit 7 of the Program Status Register (CSPR)
     * must be cleared to 0. See pp. 2-15 to 2-17 of the DDI0222 for more details.
     * The CSPR can only be accessed using assembler.
     */
    
    __asm volatile("MRS r0, cpsr");       /* Read in the CPSR register. */
    __asm volatile("BIC r0, r0, #0x80");  /* Clear bit 8, (0x80) -- Causes IRQs to be enabled. */
    __asm volatile("MSR cpsr_c, r0");     /* Write it back to the CPSR register */
}


/**
 * Disable CPU's IRQ mode that handles IRQ interrupr requests.
 */
void irq_disableIrqMode(void) 
{
    /*
     * To disable IRQ mode, bit 7 of the Program Status Register (CSPR)
     * must be set t1 0. See pp. 2-15 to 2-17 of the DDI0222 for more details.
     * The CSPR can only be accessed using assembler.
     */
    
    __asm volatile("MRS r0, cpsr");       /* Read in the CPSR register. */
    __asm volatile("ORR r0, r0, #0x80");  /* Set bit 8, (0x80) -- Causes IRQs to be disabled. */
    __asm volatile("MSR cpsr_c, r0");     /* Write it back to the CPSR register. */
}


/* a prototype required for __irq_dummyISR() */
extern void uart_print(uint8_t nr, char* str);

/*
 * A dummy ISR routine for servicing vectored IRQs.
 * 
 * It is supposed to be set as a default address of all vectored IRQ requests. If an "unconfigured"
 * IRQ is triggered, it is still better to be serviced by this dummy function instead of
 * being directed to an arbitrary address with possibly dangerous effects.
 */
static void __irq_dummyISR(void)
{
    /* 
     * An "empty" function. 
     * As this is a test aplication, it emits a warning to the UART0.
     */
     uart_print(0, "<WARNING, A DUMMY ISR ROUTINE!!!>\r\n");
}


/*
 * A dummy ISR routine for servicing nonvectored IRQs.
 * 
 * It is supposed to be set as a default address of all nonvectored IRQ requests. If an "unconfigured"
 * IRQ is triggered, it is still better to be serviced by this dummy function instead of
 * being directed to an arbitrary address with possibly dangerous effects.
 * 
 * @param param - ignored
 */
static void __irq_dummyNvISR(void* param)
{
   /* typically it should do the same as the function for unspecified vectored IRQs: */
    __irq_dummyISR();
}


/*
 * Default handler of vectored IRQs. Typically the address of this function should be 
 * set as a default value to pPicReg->VICDEFVECTADDR. It handles IRQs whose ISRs are note
 * entered into vectored registers. It is very similar to nen vectored handling of IRQs.
 */
static void __defaultVectorIsr(void)
{
    uint8_t cntr;
    
    /*
     * TODO should scanning of the priority table start at 16 or maybe at 0 ????
     * 
     * The current implementation assumes that the first 16 entries are properly serviced
     * and also enabled in their respective VICVECTCNTLn registers. 
     */
    for ( cntr=NR_VECTORS; cntr<NR_INTERRUPTS; ++cntr )
    {
        if ( __irqVect[cntr].irq>=0 && 
             __irqVect[cntr].irq<NR_INTERRUPTS &&
             pPicReg->VICINTENABLE & (UL1 << __irqVect[cntr].irq) )
        {
            ( *__irqVect[cntr].isr )();
            break;  /* out of for cntr */
        }
    }  /* for cntr */
    
    /*
     * The current implementation executes one ISR per call of this function.
     * If no appropriate ISR can be found, execute a dummy ISR.
     */
    if ( cntr >= NR_INTERRUPTS )
    {
        __irq_dummyISR();
    }
}


/*
 * IRQ handler routine, called directly from the IRQ vector, implemented in exception.c
 * Prototype of this function is not public and should not be exposed in a .h file. Instead,
 * its prototype must be declared as 'extern' where required (typically in exception.c only).
 *
 * NOTE:
 * There is no check that provided addresses are correct! It is up to developers
 * that valid ISR addresses are assigned before the IRQ mode is enabled!
 * 
 * It supports two modes of IRQ handling, vectored and nonvectored mode. They are implemented 
 * for testing purposes only, in a real world application, only one mode should be selected 
 * and implemented.
 */
void _pic_IrqHandler(void)
{
    if ( !__irq_vector_mode )
    {
        /*
         * Non vectored implementation, a.k.a. "Simple interrupt flow", described
         * on page 2-9 of DDI0181.
         */

        uint8_t i;
        
        /*
         * Traverse the priority table, for each IRQ check, whether its corresponding
         * bit in VICIRQSTATUS is set.
         */
        for ( i=0; i<NR_INTERRUPTS; ++i )
        {
            /*
             * Stop scanning the table if an invalid IRQ is found.
             */
            if ( __isrNV[i].irq<0 || __isrNV[i].irq>=NR_INTERRUPTS )
            {
                break;  /* out of for i */
            }
            

            if ( pPicReg->VICIRQSTATUS & (UL1<<__isrNV[i].irq) )
            {
	        /*
                 * The irq'th bit is set, call its service routine:
                 */ 
                ( *__isrNV[i].isr )( __isrNV[i].param );
            }
        }  /* for i */
        
        /* Call the dummy ISR if the IRQ has not been found: */
        if ( i >= NR_INTERRUPTS )
        {
            __irq_dummyNvISR(NULL);
        }
        
    }  /* if non vectored mode */
    else
    {
        /*
         * Vectored implementation, a.k.a. "Vectored interrupt flow sequence", described
         * on page 2-9 of DDI0181.
         */

        pVectoredIsrPrototype isrAddr;
        
        /* 
         * Reads the Vector Address Register with the ISR address of the currently active interrupt.
         * Reading this register also indicates to the priority hardware that the interrupt 
         * is being serviced.
         */
        isrAddr = (pVectoredIsrPrototype) pPicReg->VICVECTADDR;
	
        /* Execute the routine at the vector address */
        (*isrAddr)();
        
        /* 
         * Writes an arbitrary value to the Vector Address Register. This indicates to the
         * priority hardware that the interrupt has been serviced. 
         */
        pPicReg->VICVECTADDR = 0xFFFFFFFF;
    } /* else */
}


/**
 * Registers an interrupt service routine (ISR) for the specified IRQ request.
 * It is applicable for non-vectored IRQ handling only!
 * 
 * Nothing is done if either 'irq' (equal or greater than 32) or 'addr' (equal to NULL)
 * is invalid.
 * 
 * Entries are internally sorted in descending order by priority.
 * Entries with the same priority are additionally sorted by the time of registration
 * (entries registered earlier are ranked higher).
 * If 'irq' has already been registered, its internal entry will be overriden with 
 * new values and resorted by priority.
 *
 * @note IRQ handling should be completely disabled prior to calling this function!
 * @note 'param' should not point to data in stack unless you really know what you are doing!
 * 
 * @param irq - IRQ request number (must be smaller than 32)
 * @param addr - address of the ISR that services the interrupt 'irq'
 * @param param - void* casted pointer to function's parameter(s) (may be NULL if not applicable)
 * @param priority - priority of handling this IRQ (higher value means higher priority), the actual priority
 *                   will be silently truncated to 127 if this value is exceeded.
 *
 * @return position of the IRQ handling entry within an internal table, a negative value if registration was unsuccessful
 */ 
int8_t pic_registerNonVectoredIrq( 
                                 uint8_t irq,
                                 pNonVectoredIsrPrototype addr,
                                 void* param,
                                 uint8_t priority )
{
    const uint8_t prior = priority & 0x7F;
    int8_t irqPos = -1;
    int8_t prPos = -1;
    int8_t i;
    
    /* sanity check */
    if ( irq>=NR_INTERRUPTS || NULL==addr )
    {
        return -1;
    }
    
    /*
     * The priority table is traversed and two values are obtained:
     * - irqPos: index of the existing 'irq' or the first "empty" line
     * - prPos: index of the first entry whose priority is not larger or equal than 'prior'
     * The entry will be inserted into prPos, prior to that, all entries between 'irqPos and 'prPos'
     * will be moved one line up or down.
     */
    for ( i=0; i<NR_INTERRUPTS; ++i )
    {
        if ( irqPos<0 &&  (__isrNV[i].irq<0 || __isrNV[i].irq==irq) )
        {
            irqPos = i;
        }
        
        if ( prPos<0 && (__isrNV[i].priority<0 || __isrNV[i].priority<prior) )
        {
            prPos = i;
        }
    }

    /* just in case, should never occur */    
    if ( irqPos>=NR_INTERRUPTS || irqPos<0 || prPos<0 )
    {
        return -1;
    }
    
    /* if prPos is less than irqPos, move all intermediate entries one line down */
    if ( irqPos > prPos )
    {
        for ( i=irqPos; i>prPos; --i )
        {
            __isrNV[i] = __isrNV[i-1];
        }
    }
    
    /* if prPos is greater than irqPos, move all intermediate entries one line up... */
    if ( prPos > irqPos )
    {
        /* however this does not include the entry at prPos, whose priority is less than prior!!!*/
        --prPos;
        
        for ( i=irqPos; i<prPos; ++i )
        {
            __isrNV[i] = __isrNV[i+1];
        }
    }
    
    /* finally fill the entry at 'prPos' with the input values */
    __isrNV[prPos].irq = irq;
    __isrNV[prPos].isr = addr;
    __isrNV[prPos].param = param;
    __isrNV[prPos].priority = prior;
    
    return prPos;
}


/**
 * Unregisters an interrupt servicing routine (ISR) for the specified IRQ request
 * and removes it from the internal table of ISR routines.
 *
 * It is applicable for non-vectored IRQ handling only!
 * 
 * Nothing is done if either irq (equal or greater than 32) is invalid or it has not
 * been registered before.
 * 
 * @note IRQ handling should be completely disabled prior to calling this function!
 * 
 * @param irq - IRQ request number (must be smaller than 32)
 */
void pic_unregisterNonVectoredIrq(uint8_t irq)
{
    uint8_t pos;
    
    /* sanity check */
    if ( irq >= NR_INTERRUPTS )
    {
        return;
    }
    
    /* Find the 'irq' in the priority table: */
    for ( pos=0; pos<NR_INTERRUPTS; ++pos )
    {
        if ( __isrNV[pos].irq == irq )
        {
            break;  /* out of for pos */
        }
    }
    
    /* Nothing to do if IRQ has not been found: */
    if ( pos>=NR_INTERRUPTS )
    {
        return;
    }
    
    /* 
     * Shift all entries past 'pos' (including invalid ones) one line up.
     * This will override the entry at 'pos'.     
     */
    for ( ; pos<NR_INTERRUPTS-1; ++pos )
    {
        __isrNV[pos] = __isrNV[pos+1];
    }
    
    /* And "clear" the last entry to "default" values (see also pic_init()): */
    __isrNV[NR_INTERRUPTS-1].irq = -1;                    /* no IRQ assigned */
    __isrNV[NR_INTERRUPTS-1].isr = &__irq_dummyNvISR;     /* dummy ISR routine */
    __isrNV[NR_INTERRUPTS-1].param = NULL;                /* no paramater */
    __isrNV[NR_INTERRUPTS-1].priority = -1;               /* lowest priority */
}


/**
 * Initializes the primary interrupt controler to default settings.
 *
 * All interrupt request lines are set to generate IRQ interrupts and all
 * interrupt request lines are disabled by default. Additionally, all vector 
 * and other registers are cleared.
 */
void pic_init(void)
{
    uint8_t i;
    
    /* All interrupt request lines generate IRQ interrupts: */
    pPicReg->VICINTSELECT = 0x00000000;
    
    /* Disable all interrupt request lines: */
    pPicReg->VICINTENCLEAR = 0xFFFFFFFF;
    
    /* Clear all software generated interrupts: */
    pPicReg->VICSOFTINTCLEAR = 0xFFFFFFFF;
    
    /* Reset the default vector address: */
    pPicReg->VICDEFVECTADDR = (uint32_t) &__defaultVectorIsr;
    
    /* clear all vectored ISR addresses: */
    for ( i=0; i<NR_INTERRUPTS; ++i )
    {
        /* clear its entry in the table */
        __irqVect[i].irq = -1;                 /* no IRQ assigned */
        __irqVect[i].isr = &__irq_dummyISR;    /* dummy ISR routine */
        __irqVect[i].priority = -1;            /* lowest priority */
        
        if ( i<NR_VECTORS )
        {
            /* clear its control register */
            pPicReg->VICVECTCNTLn[i] = 0x00000000;
            /* and clear its ISR address to a dummy function */
            pPicReg->VICVECTADDRn[i] = (uint32_t) &__irq_dummyISR;
        }
    }
    
    /* clear all nonvectored ISR addresses: */
    for ( i=0; i<NR_INTERRUPTS; ++i )
    {
        __isrNV[i].irq = -1;                 /* no IRQ assigned */
        __isrNV[i].isr = &__irq_dummyNvISR;  /* dummy ISR routine */
        __isrNV[i].param = NULL;             /* no paramater */ 
        __isrNV[i].priority = -1;            /* lowest priority */
    }
    
    /* set IRQ handling to non vectored mode */
    __irq_vector_mode = 0;
}


/**
 * Enable the the interrupt request line on the PIC for the specified interrupt number.
 *
 * Nothing is done if 'irq' is invalid, i.e. equal or greater than 32.
 *
 * @param irq - interrupt number (must be smaller than 32)
 */
void pic_enableInterrupt(uint8_t irq)
{
    /* TODO check for valid (unreserved) interrupt numbers? Applies also for other functions */
    
    if ( irq < NR_INTERRUPTS )
    {
        /* See description of VICINTENABLE, page 3-7 of DDI0181: */
        pPicReg->VICINTENABLE |= ( UL1 << irq );
        
        /* Only the bit for the requested interrupt source is modified. */
    }
}


/**
 * Disable the the interrupt request line on the PIC for the specified interrupt number.
 *
 * Nothing is done if 'irq' is invalid, i.e. equal or greater than 32.
 *
 * @param irq - interrupt number (must be smaller than 32)
 */
void pic_disableInterrupt(uint8_t irq)
{
    if ( irq < NR_INTERRUPTS )
    {
        /* 
         * VICINTENCLEAR is a write only register and any attempt of reading it
         * will result in a crash. For that reason, operators as |=, &=, etc.
         * are not permitted and only direct setting of it (using operator =)
         * is possible. This is not a problem anyway as only 1-bits disable their
         * corresponding IRQs while 0-bits do not affect their corresponding
         * interrupt lines.
         * 
         * For more details, see description of VICINTENCLEAR on page 3-7 of DDI0181.
         */
        pPicReg->VICINTENCLEAR = ( UL1 << irq );
    }
}


/**
 * Disable all interrupt request lines of the PIC.
 */
void pic_disableAllInterrupts(void)
{
    /* 
     * See description of VICINTENCLEAR, page 3-7 of DDI0181.
     * All 32 bits of this register are set to 1.     
     */
    pPicReg->VICINTENCLEAR = 0xFFFFFFFF;
}


/**
 * Checks whether the interrupt request line for the requested interrupt is enabled.
 *
 * 0 is returned if 'irq' is invalid, i.e. equal or greater than 32.
 *
 * @param irq - interrupt number (must be smaller than 32)
 *
 * @return 0 if disabled, a nonzero value (typically 1) if the interrupt request line is enabled
 */
int8_t pic_isInterruptEnabled(uint8_t irq)
{
    /* See description of VICINTENCLEAR, page 3-7 of DDI0181: */

    return ( irq<NR_INTERRUPTS && (pPicReg->VICINTENABLE & (UL1<<irq)) ? 1 : 0 );
}


/**
 * What type (IRQ or FIQ) is the requested interrupt of?
 *
 * 0 is returned if 'irq' is invalid, i.e. equal or greater than 32.
 *
 * @param irq - interrupt number (must be smaller than 32)
 *
 * @return 0 if irq's type is FIQ, a nonzero value (typically 1) if the irq's type is IRQ 
 */
int8_t pic_getInterruptType(uint8_t irq)
{
    /* 
     * See description of VICINTSELECT, page 3-7 of DDI0181.
     *
     * If the corresponding bit is set to 1, the interrupt's type is FIQ,
     * otherwise it is IRQ.
     */
     
    return ( irq<NR_INTERRUPTS && 0==(pPicReg->VICINTSELECT & UL1<<irq) ? 1 : 0 );
}


/**
 * Set the requested interrupt to the desired type (IRQ or FIQ).
 *
 * Nothing is done if 'irq' is invalid, i.e. equal or greater than 32.
 *
 * @param irq - interrupt number (must be smaller than 32)
 * @param toIrq - if 0, set the interrupt's type to FIQ, otherwise set it to IRQ
 */
void pic_setInterruptType(uint8_t irq, int8_t toIrq)
{
    if (irq<NR_INTERRUPTS)
    {
    
        /*
         * Only the corresponding bit must be modified, all other bits must remain unmodified.
         * For that reason, appropriate bitwise operators are applied.
         *
         * The interrupt's type is set via VICINTSELECT. See description
         * of the register at page 3-7 of DDI0181.
         */
        if ( toIrq )
        {
            /* Set the corresponding bit to 0 by bitwise and'ing bitmask's zero complement */
            pPicReg->VICINTSELECT &= ~(UL1 << irq);
        }
        else
        {
            /* Set the corresponding bit to 1 by bitwise or'ing the bitmask */
            pPicReg->VICINTSELECT |= (UL1 << irq);
        }
    }
}


/**
 * Assigns the default vector address (VICDEFVECTADDR).
 *
 * Nothing is done, if 'addr' is invalid, i.e. NULL
 * 
 * @param addr - address od the default ISR
 */ 
void pic_setDefaultVectorAddr(pVectoredIsrPrototype addr)
{
    if ( NULL != addr )
    {
        pPicReg->VICDEFVECTADDR = (uint32_t) addr;
    }
}


/**
 * Registers a vector interrupt ISR for the requested interrupt request line.
 * The vectored interrupt is enabled by default.
 *
 * Nothing is done and -1 is returned if either 'irq' is invalid (must be less than 32) 
 * or ISR's address is NULL.
 *
 * Entries are internally sorted in descending order by priority.
 * Entries with the same priority are additionally sorted by the time of registration
 * (entries registered earlier are ranked higher).
 * If 'irq' has already been registered, its internal entry will be overriden with
 * new values and resorted by priority.
 * The first 16 entries, sorted by priority, are automatically entered into appropriate vector
 * registers of the primary interrupt controller.
 * 
 * @note IRQ handling should be completely disabled prior to calling this function!
 * 
 * @param irq - interrupt number (must be smaller than 32)
 * @param addr - address of the ISR that services the interrupt 'irq'
 * @param priority - priority of handling this IRQ (higher value means higher priority), the actual priority
 *                   will be silently truncated to 127 if this value is exceeded.
 *
 * @return position of the IRQ handling entry within an internal table, a negative value if registration was unsuccessful
 */
int8_t pic_registerVectorIrq(
                              uint8_t irq, 
                              pVectoredIsrPrototype addr,
                              uint8_t priority )
{
    const prior = priority & 0x7F;
    int8_t irqPos = -1;
    int8_t prPos = -1;
    int8_t i;
    
    /* sanity check: */
    if (irq>=NR_INTERRUPTS || NULL==addr )
    {
        return -1;
    }
      
    /*
     * The priority table is traversed and two values are obtained:
     * - irqPos: index of the existing 'irq' or the first "empty" line
     * - prPos: index of the first entry whose priority is not larger or equal than 'prior'
     * The entry will be inserted into prPos, prior to that, all entries between 'irqPos and 'prPos'
     * will be moved one line up or down.
     */
    
    for ( i=0; i<NR_INTERRUPTS; ++i )
    {
        if ( irqPos<0 && (__irqVect[i].irq<0 || __irqVect[i].irq==irq) )
        {
            irqPos = i;
        }
        
        if ( prPos<0 && (__irqVect[i].priority<0 || __irqVect[i].priority<prior) )
        {
            prPos = i;
        }
    }  /* for i */
    
    /* just in case, should never occur */
    if ( irqPos>=NR_INTERRUPTS || irqPos<0 || prPos<0 )
    {
        return -1;
    }
    
    /* if prPos is less than irqPos, move all intermediate entries one line down */
    if ( irqPos > prPos )
    {
        for ( i=irqPos; i>prPos; --i )
        {
            __irqVect[i] = __irqVect[i-1];
            
            /* for i<16 also update PIC's vector address and control registers */
            if ( i<NR_VECTORS )
            {
                if ( __irqVect[i].irq >= 0 )
                {
                    pPicReg->VICVECTCNTLn[i] = __irqVect[i].irq | BM_VECT_ENABLE_BIT;
                    pPicReg->VICVECTADDRn[i] = (uint32_t) __irqVect[i].isr;
                }
                else
                {
                    /* if i^th line is "empty", clear the appropriate vector registers */
                    pPicReg->VICVECTCNTLn[i] = 0x00000000;
                    pPicReg->VICVECTADDRn[i] = (uint32_t) &__irq_dummyISR;
                }
            }  /* if i < NR_VECTORS */
        }  /* for i*/
    }  /* if irqPos > prPos */
    
    /* if prPos is greater than irqPos, move all intermediate entries one line up... */
    if ( prPos > irqPos )
    {
        /* however this does not include the entry at prPos, whose priority is less than prior!!!*/
        --prPos;
        
        for ( i=irqPos; i<prPos; ++i )
        {
            __irqVect[i] = __irqVect[i+1];
            
            /* for i<16 also update PIC's vector address and control registers */
            if ( i<NR_VECTORS )
            {
                if ( __irqVect[i].irq >= 0 )
                {
                    pPicReg->VICVECTCNTLn[i] = __irqVect[i].irq | BM_VECT_ENABLE_BIT;
                    pPicReg->VICVECTADDRn[i] = (uint32_t) __irqVect[i].isr;
                }
                else
                {
                    /* if i^th line is "empty", clear the appropriate vector registers */
                    pPicReg->VICVECTCNTLn[i] = 0x00000000;
                    pPicReg->VICVECTADDRn[i] = (uint32_t) &__irq_dummyISR;
                }
            }  /* if i < NR_VECTORS */
        }  /* for i */
    }  /* if prPos > irqPos */
    
    /* finally fill the entry at 'prPos' with the input values */
    __irqVect[prPos].irq = irq;
    __irqVect[prPos].isr = addr;
    __irqVect[prPos].priority = prior;
    
    /* if prPos<16 also update the appropriate vector registers */
    if ( prPos < NR_VECTORS )
    {
        if ( irq >= 0 )
        {
            pPicReg->VICVECTCNTLn[prPos] = irq | BM_VECT_ENABLE_BIT;
            pPicReg->VICVECTADDRn[prPos] = (uint32_t) addr;
        }
        else
        {
            pPicReg->VICVECTCNTLn[prPos] = 0x00000000;
            pPicReg->VICVECTADDRn[prPos] = (uint32_t) &__irq_dummyISR;
        }
    }
    
    
    return prPos;
}


/**
 * Unregisters a vector interrupt ISR for the requested interrupt request line.
 *
 * Nothing is done if 'irq' is invalid, i.e. equal or greater than 32 or
 * no vector for the 'irq' exists.
 *
 * @note IRQ handling should be completely disabled prior to calling this function!
 * 
 * @param irq - interrupt number (must be smaller than 32)
 */
void pic_unregisterVectorIrq(uint8_t irq)
{
    uint8_t pos;
    
    /* sanity check */
    if (irq>=NR_INTERRUPTS)
    {
        return;
    }
    
    
    /* Find the 'irq' in the priority table: */
    for ( pos=0; pos<NR_INTERRUPTS; ++pos )
    {
        if ( __irqVect[pos].irq == irq )
        {
            break; /* out of for pos */
        }
    }
    
    /* Nothing to do if IRQ has not been found: */
    if ( pos>=NR_INTERRUPTS )
    {
        return;
    }
    
    /*
     * Shift all entries past 'pos' (including invalid ones) one line up.
     * This will override the entry at 'pos'.
     */
    for ( ; pos<NR_INTERRUPTS-1; ++pos )
    {
        __irqVect[pos] = __irqVect[pos+1];
        
        if ( pos<NR_VECTORS )
        {
            /* for pos<16 also update PIC's vector address and control registers */
            if ( __irqVect[pos].irq >= 0 )
            {
                pPicReg->VICVECTCNTLn[pos] = __irqVect[pos].irq | BM_VECT_ENABLE_BIT;
                pPicReg->VICVECTADDRn[pos] = (uint32_t) __irqVect[pos].isr;
            }
            else
            {
                /* if pos^th line is "empty", clear the appropriate vector registers */
                pPicReg->VICVECTCNTLn[pos] = 0x00000000;
                pPicReg->VICVECTADDRn[pos] = (uint32_t) &__irq_dummyISR;
            }
        }
    }
    
    /* And "clear" the last entry to "default" values (see also pic_init()): */
    __irqVect[NR_INTERRUPTS-1].irq = -1;               /* no IRQ assigned */
    __irqVect[NR_INTERRUPTS-1].isr = &__irq_dummyISR;  /* dummy ISR routine */
    __irqVect[NR_INTERRUPTS-1].priority = -1;          /* lowest priority */
}


/**
 * Enables the vectored interrupt for the requested interrupt request line.
 *
 * Nothing is done and -1 is returned if irq is invalid (equal or greater than 32) or
 * no slot with the requested irq has been found.
 *
 * @param irq - interrupt number (must be smaller than 32)
 *
 * @return interrupt's vector slot (0 to 15) or -1 if no slot has been found
 */
int8_t pic_enableVectorIrq(uint8_t irq)
{
    int8_t cntr;
    
    if ( irq>=NR_INTERRUPTS )
    {
        return -1;
    }
    
    /* Find a slot with the requested irq: */
    for ( cntr=0; cntr<NR_VECTORS; ++cntr )
    {
        if ( irq == __irqVect[cntr].irq )
        {
            /* found, set its ENABLE bit in VICVECTCNTLn: */
            pPicReg->VICVECTCNTLn[cntr] |= BM_VECT_ENABLE_BIT;
            /* No need to search further: */
            break;  /* out of for cntr */
        }
    }  /* for cntr */
    
    return ( cntr<NR_VECTORS ? cntr : -1 );
}


/**
 * Disables the vectored interrupt for the requested interrupt request line.
 *
 * Nothing is done if irq is invalid (equal or greater than 32) or
 * no slot with the requested irq has been found.
 *
 * @param irq - interrupt number (must be smaller than 32)
 */
void pic_disableVectorIrq(uint8_t irq)
{
    uint8_t cntr;
    
    if (irq>=NR_INTERRUPTS)
    {
        return;
    }
    
    for ( cntr=0; cntr<NR_VECTORS; ++cntr )
    {
        if ( irq == __irqVect[cntr].irq )
        {
            /* found an occurrence of irq, unset its ENABLE bit in VICVECTCNTLn: */
            pPicReg->VICVECTCNTLn[cntr] &= ~BM_VECT_ENABLE_BIT;
            /* 
             * Continue the loop and disable potential other slots
             * with the same irq (should not occur in normal situatuons but
             * it does not hurt). 
             */
        }
    }
}


/**
 * Unregisters all vector interrupts.
 */
void pic_unregisterAllVectorIrqs(void)
{
    uint8_t i;
    
    /* Clear all entries in the priority table */
    for ( i=0; i<NR_VECTORS; ++i )
    {
        __irqVect[i].irq = -1;
        __irqVect[i].isr = &__irq_dummyISR;
        __irqVect[i].priority = -1;
        
        /* Clear all vector's VICVECTCNTLn and VICVECTADDRn registers: */
        if ( i<NR_VECTORS )
        {
            pPicReg->VICVECTCNTLn[i] = 0x00000000;
            pPicReg->VICVECTADDRn[i] = (uint32_t) &__irq_dummyISR;
        }
    }
}


/**
 * Disables all vectored interrupts
 */
void pic_disableAllVectorIrqs(void)
{
    uint8_t i;
    
    /* Whatever VICVECTCNTLn is set to, unset its ENABLE bit to 0 */
    for ( i=0; i<NR_VECTORS; ++i )
    {
        pPicReg->VICVECTCNTLn[i] &= ~BM_VECT_ENABLE_BIT;
    }
}


/**
 * Triggers a software generated interrupt. The chosen interrupt request line
 * must be enabled (masked) in order for the interrupt to be actually triggered.
 *
 * Nothing is done if 'irq' is invalid (equal or greater than 32).
 *
 * @note It is strongly recommended that only IRQs of disabled peripherals 
 * (or at least peripherals with disabled interrupt triggering) are passed
 * to this function. It is strongly recommended to use IRQ 1, resrved for
 * software generated interrupts.
 *
 * @param irq - interrupt number (must be smaller than 32)
 *
 * @return 'irq' if interrupt successfully set, a negative value (typically -1) otherwise
 */
int8_t pic_setSwInterruptNr(uint8_t irq)
{
    if (irq>=NR_INTERRUPTS)
    {
        return -1;
    }
    
    /* 
     * Interrupts can be software triggered via VICSOFTINT.
     * See description of the register on page 3-8 of DDI0181.
     */
    
    pPicReg->VICSOFTINT |= ( UL1 << irq );
    
    return irq;
}


/**
 * Clears an active interrupt via software.
 *
 * Nothing is done if 'irq' is invalid (equal or greater than 32) or the 
 * requested interrupt is not active.
 *
 * @note It is only recommended to use this function if the interrupt has been
 * set via software using pic_setSwInterruptNr.
 *
 * @param irq - interrupt number (must be smaller than 32)
 *
 * @return 'irq' if interrupt successfully cleared, a negative value (typically -1) otherwise
 */
int8_t pic_clearSwInterruptNr(uint8_t irq)
{
    uint32_t bitmask;
    uint8_t retVal = -1;
    
    if (irq>=NR_INTERRUPTS)
    {
        return -1;
    }
    
    /* 
     * Interrupts can be software cleared via VICSOFTINTCLEAR.
     * See description of the register on page 3-8 of DDI0181.
     */
     
     bitmask = UL1 << irq;
     
     /*
      * Before the interrupt is cleared it is checked whether it is active.
      * TODO: should VICIRQSTATUS and VICFIQSTATUS be check instead of VICRAWINTR?
      */
     if ( pPicReg->VICRAWINTR & bitmask )
     {
         /* The interrupt is active, clear it 
          * * The register is write only and should not be read. Only 1-bits clear
          * their corresponding IRQs, 0-bits have no effect on "their" IRQs.
          */

         pPicReg->VICSOFTINTCLEAR = bitmask;
         
         retVal = irq;
     }
     
     return retVal;
}


/**
 * Triggers the software generated interrupt (IRQ1).
 *
 * @return 'irq' if interrupt successfully set, a negative value (typically -1) otherwise
 */
int8_t pic_setSoftwareInterrupt(void)
{
    return pic_setSwInterruptNr(BSP_SOFTWARE_IRQ);
}


/**
 * Clears the software generated interrupt (IRQ1).
 *
 * @return 'irq' if interrupt successfully set, a negative value (typically -1) otherwise
 */
int8_t pic_clearSoftwareInterrupt(void)
{
    return pic_clearSwInterruptNr(BSP_SOFTWARE_IRQ);
}
