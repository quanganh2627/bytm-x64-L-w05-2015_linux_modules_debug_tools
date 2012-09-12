/*COPYRIGHT**
    Copyright (C) 2005-2011 Intel Corporation.  All Rights Reserved.

    This file is part of SEP Development Kit

    SEP Development Kit is free software; you can redistribute it
    and/or modify it under the terms of the GNU General Public License
    version 2 as published by the Free Software Foundation.

    SEP Development Kit is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with SEP Development Kit; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

    As a special exception, you may use this file as part of a free software
    library without restriction.  Specifically, if other files instantiate
    templates or use macros or inline functions from this file, or you compile
    this file and link it with other files to produce an executable, this
    file does not by itself cause the resulting executable to be covered by
    the GNU General Public License.  This exception does not however
    invalidate any other reasons why the executable file might be covered by
    the GNU General Public License.
**COPYRIGHT*/

/*
 *  cvs_id[] = "$Id$"
 */

#include "lwpmudrv_defines.h"
#include <linux/version.h>
#include <linux/wait.h>
#include <linux/fs.h>

#include "lwpmudrv_types.h"
#include "rise_errors.h"
#include "lwpmudrv_ecb.h"
#include "lwpmudrv_struct.h"

#include "lwpmudrv.h"
#include "utility.h"
#include "control.h"
#include "output.h"
#include "core2.h"
#include "ecb_iterators.h"
#include "pebs.h"
#include "apic.h"

extern EVENT_CONFIG   global_ec;
extern U64           *read_counter_info;
extern LBR            lbr;
extern DRV_CONFIG     pcfg;
extern PWR            pwr;

/*
 * core2_Write_PMU
 *
 *     Parameters
 *         UNUSED
 *
 *     Returns
 *         NONE
 *
 *     Description
 *         Initial write of PMU registers.
 *         Walk through the enties and write the value of the register accordingly.
 *         When current_group = 0, then this is the first time this routine is called,
 *         initialize the locks and set up EM tables.
 *
 */
static VOID
core2_Write_PMU (
    VOID  *param
)
{
    U32            this_cpu = CONTROL_THIS_CPU();
    CPU_STATE      pcpu     = &pcb[this_cpu];

    if (CPU_STATE_current_group(pcpu) == 0) {
        if (EVENT_CONFIG_mode(global_ec) != EM_DISABLED) {
            U32            index;
            U32            st_index;
            U32            j;

            /* Save all the initialization values away into an array for Event Multiplexing. */
            for (j = 0; j < EVENT_CONFIG_num_groups(global_ec); j++) {
                CPU_STATE_current_group(pcpu) = j;
                st_index   = CPU_STATE_current_group(pcpu) * EVENT_CONFIG_max_gp_events(global_ec);
                FOR_EACH_DATA_GP_REG(pecb, i) {
                    index = st_index + (ECB_entries_reg_id(pecb,i) - IA32_PMC0);
                    CPU_STATE_em_tables(pcpu)[index] = ECB_entries_reg_value(pecb,i);
                } END_FOR_EACH_DATA_GP_REG;
            }
            /* Reset the current group to the very first one. */
            CPU_STATE_current_group(pcpu) = 0;
        }
    }

    if (dispatch->hw_errata) {
        dispatch->hw_errata();
    }

    FOR_EACH_REG_ENTRY(pecb, i) {
        /*
         * Writing the GLOBAL Control register enables the PMU to start counting.
         * So write 0 into the register to prevent any counting from starting.
         */
        if (ECB_entries_reg_id(pecb,i) == IA32_PERF_GLOBAL_CTRL) {
            SYS_Write_MSR(ECB_entries_reg_id(pecb,i), 0LL);
            continue;
        }
        /*
         *  PEBS is enabled for this collection session
         */
        if (ECB_entries_reg_id(pecb,i) == IA32_PEBS_ENABLE &&
            ECB_entries_reg_value(pecb,i)) {
            SYS_Write_MSR(ECB_entries_reg_id(pecb,i), 0LL);
            continue;
        }
        if (ECB_entries_reg_id(pecb,i) == COMPOUND_CTR_CTL) {
            SYS_Write_MSR(ECB_entries_reg_id(pecb,i), 0LL);
            continue;
        }
        SYS_Write_MSR(ECB_entries_reg_id(pecb,i), ECB_entries_reg_value(pecb,i));
#if defined(MYDEBUG)
        {
            U64 val = SYS_Read_MSR(ECB_entries_reg_id(pecb,i));
            SEP_PRINT_DEBUG("Write reg 0x%x --- value 0x%llx -- read 0x%llx\n",
                            ECB_entries_reg_id(pecb,i), ECB_entries_reg_value(pecb,i), val);
        }
#endif
    } END_FOR_EACH_REG_ENTRY;

    return;
}

/*
 * core2_Disable_PMU
 *
 *     Parameters
 *         UNUSED
 *
 *     Returns
 *         NONE
 *
 *     Description
 *         Zero out the global control register.  This automatically disables
 *         the PMU counters.
 *
 */
static VOID
core2_Disable_PMU (
    PVOID  param
)
{
    SYS_Write_MSR(IA32_PERF_GLOBAL_CTRL, 0LL);
    SYS_Write_MSR(IA32_PEBS_ENABLE, 0LL);
    FOR_EACH_CCCR_REG(pecb,i) {
       if (ECB_entries_is_compound_ctr_bit_set(pecb, i)) {
           SYS_Write_MSR(COMPOUND_CTR_CTL,0LL);
       }
    } END_FOR_EACH_CCCR_REG;

    if (GLOBAL_STATE_current_phase(driver_state) != DRV_STATE_RUNNING) {
        APIC_Disable_PMI();
    }

    return;
}

/*
 * core2_Enable_PMU
 *
 *     Parameters
 *         UNUSED
 *
 *     Returns
 *         NONE
 *
 *     Description
 *         Set the enable bit for all the CCCR registers.
 *
 */
static VOID
core2_Enable_PMU (
    PVOID   param
)
{
    /*
     * Get the value from the event block
     *   0 == location of the global control reg for this block.
     *   Generalize this location awareness when possible
     */
    U32         this_cpu = CONTROL_THIS_CPU();
    CPU_STATE   pcpu     = &pcb[this_cpu];
    ECB         pecb     = PMU_register_data[CPU_STATE_current_group(pcpu)];

    if (GLOBAL_STATE_current_phase(driver_state) == DRV_STATE_RUNNING) {
        APIC_Enable_Pmi();
        SYS_Write_MSR(IA32_PERF_GLOBAL_CTRL, ECB_entries_reg_value(pecb,0));
        SYS_Write_MSR(IA32_PEBS_ENABLE, ECB_entries_reg_value(pecb,2));
        SYS_Write_MSR(IA32_DEBUG_CTRL, ECB_entries_reg_value(pecb,3));
        FOR_EACH_CCCR_REG(pecb,i) {
           if (ECB_entries_is_compound_ctr_bit_set(pecb, i)) {
                 SYS_Write_MSR(COMPOUND_CTR_CTL, ECB_entries_reg_value(pecb,i));
           }
        } END_FOR_EACH_CCCR_REG;
#if defined(MYDEBUG)
        {
            U64 val;
            val = SYS_Read_MSR(IA32_PERF_GLOBAL_CTRL);
            SEP_PRINT_DEBUG("Write reg 0x%x--- read 0x%llx\n",
                            ECB_entries_reg_id(pecb,0), SYS_Read_MSR(IA32_PERF_GLOBAL_CTRL));
        }
#endif
    }
    SEP_PRINT_DEBUG("Reenabled PMU with value 0x%llx\n", ECB_entries_reg_value(pecb,0));

    return;
}

/*
 * core2_ReInit_Data
 *
 *     Parameters
 *         UNUSED
 *
 *     Returns
 *         NONE
 *
 *     Description
 *
 *         Called by the interrupt handler to reinitialize the counters before
 *         re-enabling the collection.
 *
 */
static VOID
core2_ReInit_Data (
    PVOID param
)
{
    return;
}

/*
 * core2_Read_PMU_Data
 *
 *     Parameters
 *         UNUSED
 *
 *     Returns
 *         NONE
 *
 *     Description
 *         Read all the data MSR's into a buffer.
 *         Called by the interrupt handler.
 *
 */
static void
core2_Read_PMU_Data (
    PVOID   param
)
{
    S32       start_index, j;
    U64      *buffer    = read_counter_info;
    U32       this_cpu;
    CPU_STATE pcpu;
    ECB       pecb;

    preempt_disable();
    this_cpu  = CONTROL_THIS_CPU();
    preempt_enable();
    pcpu      = &pcb[this_cpu];
    pecb      = PMU_register_data[CPU_STATE_current_group(pcpu)];

    start_index = ECB_num_events(pecb) * this_cpu;
    SEP_PRINT_DEBUG("PMU control_data 0x%p, buffer 0x%p, j = %d\n", PMU_register_data, buffer, j);
    FOR_EACH_DATA_REG(pecb,i) {
        j = start_index + ECB_entries_event_id_index(pecb,i);
        if (ECB_entries_is_compound_ctr_sub_bit_set(pecb, i)) {
             continue;
        }
        buffer[j] = SYS_Read_MSR(ECB_entries_reg_id(pecb,i));
        SEP_PRINT_DEBUG("this_cpu %d, event_id %d, value 0x%llx\n", this_cpu, i, buffer[j]);
    } END_FOR_EACH_DATA_REG;

    return;
}

/*
 * core2_Check_Overflow_Errata
 *
 *     Parameters
 *         pecb:            The current event control block
 *         index:           index of the register to process
 *         overflow_status: current overflow mask
 *
 *     Returns
 *         Updated Event mask of the overflowed registers.
 *
 *     Description
 *         fixed_counter1 on some architectures gets interfered by 
 *         other event counts.  Overcome this problem by reading the
 *         counter value and resetting the overflow mask.
 *
 */
static U64
core2_Check_Overflow_Errata (
    ECB   pecb,
    U32   index,
    U64   overflow_status
)
{
    if (ECB_entries_reg_id(pecb, index) == IA32_FIXED_CTR1 &&
        (overflow_status & 0x200000000LL) == 0LL) {
        U64 val = SYS_Read_MSR(IA32_FIXED_CTR1);
        val &= ECB_entries_max_bits(pecb,index);
        if (val < ECB_entries_reg_value(pecb, index)) {
            overflow_status |= 0x200000000LL;
            SEP_PRINT_DEBUG("Reset -- clk count %llx, status %llx\n", val, overflow_status);
        }
    }

    return overflow_status;
}

/*
 * core2_Check_Overflow
 *
 *     Parameters
 *         INOUT masks
 *
 *     Returns
 *         Event mask of the overflowed registers.
 *
 *     Description
 *         Called by the data processing method to figure out
 *         which registers have overflowed.
 *         Does not include Nehalem and Bonnell specific code
 *         Uncore also does not exist.
 *
 */
static void
core2_Check_Overflow (
    DRV_MASKS    masks
)
{
    U32              index;
    U64              overflow_status     = 0;
    U32              this_cpu            = CONTROL_THIS_CPU();
    BUFFER_DESC      bd                  = &cpu_buf[this_cpu];
    CPU_STATE        pcpu                = &pcb[this_cpu];
    ECB              pecb                = PMU_register_data[CPU_STATE_current_group(pcpu)];
    U64              overflow_status_clr = 0;
    DRV_EVENT_MASK_NODE event_flag;

    // initialize masks 
    DRV_MASKS_masks_num(masks) = 0;

    overflow_status = SYS_Read_MSR(IA32_PERF_GLOBAL_STATUS);

    if (DRV_CONFIG_pebs_mode(pcfg)) {
        overflow_status = PEBS_Overflowed (this_cpu, overflow_status);
    }
    overflow_status_clr = overflow_status;

    if (dispatch->check_overflow_gp_errata) {
        overflow_status = dispatch->check_overflow_gp_errata(pecb,  &overflow_status_clr);
    }
    SEP_PRINT_DEBUG("Overflow:  cpu: %d, status 0x%llx \n", this_cpu, overflow_status);
    index                        = 0;
    BUFFER_DESC_sample_count(bd) = 0;
    FOR_EACH_DATA_REG(pecb, i) {
        if (ECB_entries_fixed_reg_get(pecb, i)) {
            index = ECB_entries_reg_id(pecb, i) - IA32_FIXED_CTR0 + 0x20;
            if (dispatch->check_overflow_errata) {
                overflow_status = dispatch->check_overflow_errata(pecb, i, overflow_status);
            }
        }
        else if (ECB_entries_is_compound_ctr_sub_bit_set(pecb, i)) {
            continue;
        }
        else if (ECB_entries_is_compound_ctr_bit_set(pecb, i) && !DRV_CONFIG_event_based_counts(pcfg))  {
            index = COMPOUND_CTR_OVF_SHIFT;
        }
        else if (ECB_entries_is_gp_reg_get(pecb, i)) {
            index = ECB_entries_reg_id(pecb, i) - IA32_PMC0;
        }
        else {
            continue;
        }
        if (overflow_status & ((U64)1 << index)) {
            SEP_PRINT_DEBUG("Overflow:  cpu: %d, index %d\n", this_cpu, index);
            SEP_PRINT_DEBUG("register 0x%x --- val 0%llx\n",
                            ECB_entries_reg_id(pecb,i),
                            SYS_Read_MSR(ECB_entries_reg_id(pecb,i)));
            SYS_Write_MSR(ECB_entries_reg_id(pecb,i), ECB_entries_reg_value(pecb,i));

            DRV_EVENT_MASK_bitFields1(&event_flag) = (U8) 0;
            if (ECB_entries_precise_get(pecb, i)) {
                DRV_EVENT_MASK_precise(&event_flag) = 1;
            }
            if (ECB_entries_lbr_value_get(pecb, i)) {
                DRV_EVENT_MASK_lbr_capture(&event_flag) = 1;
            }
            if (ECB_entries_uncore_get(pecb, i)) {
                DRV_EVENT_MASK_uncore_capture(&event_flag) = 1;
            }

            if (DRV_MASKS_masks_num(masks) < MAX_OVERFLOW_EVENTS) {
                DRV_EVENT_MASK_bitFields1(DRV_MASKS_eventmasks(masks) + DRV_MASKS_masks_num(masks)) = DRV_EVENT_MASK_bitFields1(&event_flag);
                DRV_EVENT_MASK_event_idx(DRV_MASKS_eventmasks(masks) + DRV_MASKS_masks_num(masks)) = ECB_entries_event_id_index(pecb, i);
                DRV_MASKS_masks_num(masks)++;
            } 
            else {
                SEP_PRINT_ERROR("The array for event masks is full.\n");
            }

            SEP_PRINT_DEBUG("overflow -- 0x%llx, index 0x%llx\n", overflow_status, (U64)1 << index);
            SEP_PRINT_DEBUG("slot# %d, reg_id 0x%x, index %d\n",
                            i, ECB_entries_reg_id(pecb, i), index);
            if (ECB_entries_event_id_index(pecb, i) == CPU_STATE_trigger_event_num(pcpu)) {
                CPU_STATE_trigger_count(pcpu)--;
            }
        }
    } END_FOR_EACH_DATA_REG;


    // Reinitialize the global overflow control register
    SYS_Write_MSR(IA32_PERF_GLOBAL_OVF_CTRL, overflow_status_clr);

    SEP_PRINT_DEBUG("Check Overflow completed %d\n", this_cpu);
}

/*
 * core_Swap_Group
 *
 *     Parameters
 *         IN restart
 *
 *     Returns
 *         NONE
 *
 *     Description
 *         Swap function for event multiplexing.
 *         Freeze the counting.
 *         Swap the groups.
 *         Enable the counting.
 *
 */
static VOID
core2_Swap_Group (
    DRV_BOOL  restart
)
{
    U32            index;
    U32            next_group;
    U32            st_index;
    U32            this_cpu = CONTROL_THIS_CPU();
    CPU_STATE      pcpu     = &pcb[this_cpu];

    st_index   = CPU_STATE_current_group(pcpu) * EVENT_CONFIG_max_gp_events(global_ec);
    next_group = (CPU_STATE_current_group(pcpu) + 1);
    if (next_group >= EVENT_CONFIG_num_groups(global_ec)) {
        next_group = 0;
    }

    SEP_PRINT_DEBUG("current group : 0x%x\n", CPU_STATE_current_group(pcpu));
    SEP_PRINT_DEBUG("next group : 0x%x\n", next_group);

    // Save the counters for the current group
    if (!DRV_CONFIG_event_based_counts(pcfg)) {
        FOR_EACH_DATA_GP_REG(pecb, i) {
            index = st_index + (ECB_entries_reg_id(pecb, i) - IA32_PMC0);
            CPU_STATE_em_tables(pcpu)[index] = SYS_Read_MSR(ECB_entries_reg_id(pecb,i));
            SEP_PRINT_DEBUG("Saved value for reg 0x%x : 0x%llx ",
                            ECB_entries_reg_id(pecb,i),
                            CPU_STATE_em_tables(pcpu)[index]);
        } END_FOR_EACH_DATA_GP_REG;
    }

    CPU_STATE_current_group(pcpu) = next_group;

    if (dispatch->hw_errata) {
        dispatch->hw_errata();
    }

    // First write the GP control registers (eventsel)
    FOR_EACH_CCCR_GP_REG(pecb, i) {
        SYS_Write_MSR(ECB_entries_reg_id(pecb,i), ECB_entries_reg_value(pecb,i));
    } END_FOR_EACH_CCCR_GP_REG;

    if (DRV_CONFIG_event_based_counts(pcfg)) {
        // In EBC mode, reset the counts for all events except for trigger event
        FOR_EACH_DATA_REG(pecb, i) {
            if (ECB_entries_event_id_index(pecb, i) != CPU_STATE_trigger_event_num(pcpu)) {
                SYS_Write_MSR(ECB_entries_reg_id(pecb,i), 0LL);
            }
        } END_FOR_EACH_DATA_REG;
    }
    else {
        // Then write the gp count registers
        st_index = CPU_STATE_current_group(pcpu) * EVENT_CONFIG_max_gp_events(global_ec);
        FOR_EACH_DATA_GP_REG(pecb, i) {
            index = st_index + (ECB_entries_reg_id(pecb, i) - IA32_PMC0);
            SYS_Write_MSR(ECB_entries_reg_id(pecb,i), CPU_STATE_em_tables(pcpu)[index]);
            SEP_PRINT_DEBUG("Restore value for reg 0x%x : 0x%llx ",
                            ECB_entries_reg_id(pecb,i),
                            CPU_STATE_em_tables(pcpu)[index]);
        } END_FOR_EACH_DATA_GP_REG;
    }

    FOR_EACH_ESCR_REG(pecb,i) {
        SYS_Write_MSR(ECB_entries_reg_id(pecb, i),ECB_entries_reg_value(pecb, i));
    } END_FOR_EACH_ESCR_REG;

    /*
     *  reset the em factor when a group is swapped
     */
    CPU_STATE_trigger_count(pcpu) = EVENT_CONFIG_em_factor(global_ec);

    return;
}

/*
 * core2_Initialize
 *
 *     Parameters
 *         UNUSED
 *
 *     Returns
 *         NONE
 *
 *     Description
 *         Saves the relevant PMU state (minimal set of MSRs required
 *         to avoid conflicts with other Linux tools, such as Oprofile).
 *         This function should be called in parallel across all CPUs
 *         prior to the start of sampling, before PMU state is changed.
 *
 */
static VOID
core2_Initialize (
    VOID  *param
)
{
    U32        this_cpu = CONTROL_THIS_CPU();
    CPU_STATE  pcpu;

    SEP_PRINT_DEBUG("Inside core2_Initialize\n");

    if (pcb == NULL) {
        return;
    }

    pcpu  = &pcb[this_cpu];
    CPU_STATE_pmu_state(pcpu) = pmu_state + (this_cpu * 2);
    if (CPU_STATE_pmu_state(pcpu) == NULL) {
        SEP_PRINT_WARNING("Unable to save PMU state on CPU %d\n",this_cpu);
        return;
    }

    // save the original PMU state on this CPU (NOTE: must only be called ONCE per collection)
    CPU_STATE_pmu_state(pcpu)[0] = SYS_Read_MSR(IA32_DEBUG_CTRL);
    CPU_STATE_pmu_state(pcpu)[1] = SYS_Read_MSR(IA32_PERF_GLOBAL_CTRL);

    SEP_PRINT_DEBUG("Saving PMU state on CPU %d :\n", this_cpu);
    SEP_PRINT_DEBUG("    msr_val(IA32_DEBUG_CTRL)=0x%llx \n", CPU_STATE_pmu_state(pcpu)[0]);
    SEP_PRINT_DEBUG("    msr_val(IA32_PERF_GLOBAL_CTRL)=0x%llx \n", CPU_STATE_pmu_state(pcpu)[1]);

    return;
}

/*
 * core2_Destroy
 *
 *     Parameters
 *         UNUSED
 *
 *     Returns
 *         NONE
 *
 *     Description
 *         Restores the previously saved PMU state done in core2_Initialize.
 *         This function should be called in parallel across all CPUs
 *         after sampling collection ends/terminates.
 *
 */
static VOID
core2_Destroy (
    VOID *param
)
{
    U32        this_cpu;
    CPU_STATE  pcpu;

    SEP_PRINT_DEBUG("Inside core2_Destroy\n");

    if (pcb == NULL) {
        return;
    }

    preempt_disable();
    this_cpu = CONTROL_THIS_CPU();
    preempt_enable();
    pcpu = &pcb[this_cpu];

    if (CPU_STATE_pmu_state(pcpu) == NULL) {
        SEP_PRINT_WARNING("Unable to restore PMU state on CPU %d\n",this_cpu);
        return;
    }

    SEP_PRINT_DEBUG("Restoring PMU state on CPU %d :\n", this_cpu);
    SEP_PRINT_DEBUG("    msr_val(IA32_DEBUG_CTRL)=0x%llx \n", CPU_STATE_pmu_state(pcpu)[0]);
    SEP_PRINT_DEBUG("    msr_val(IA32_PERF_GLOBAL_CTRL)=0x%llx \n", CPU_STATE_pmu_state(pcpu)[1]);

    // restore the previously saved PMU state 
    // (NOTE: assumes this is only called ONCE per collection)
    SYS_Write_MSR(IA32_DEBUG_CTRL, CPU_STATE_pmu_state(pcpu)[0]);
    SYS_Write_MSR(IA32_PERF_GLOBAL_CTRL, CPU_STATE_pmu_state(pcpu)[1]);

    CPU_STATE_pmu_state(pcpu) = NULL;

    return;
}

/*
 * @FN core2_Read_LBRs(buffer)
 *
 * @param   IN buffer - pointer to the buffer to write the data into
 * @return  None
 *
 * @brief   Read all the LBR registers into the buffer provided and return
 *
 */
static VOID
core2_Read_LBRs (
    VOID   *buffer
)
{
    U32  i;
    U64 *lbr_buf = (U64 *)buffer;

    SEP_PRINT_DEBUG("Inside core2_Read_LBRs\n");
    for (i = 0; i < LBR_num_entries(lbr); i++) {
        *lbr_buf = SYS_Read_MSR(LBR_entries_reg_id(lbr,i));
        lbr_buf++;
    }

    return;
}

static VOID
core2_Clean_Up (
    VOID   *param
)
{
    FOR_EACH_REG_ENTRY(pecb, i) {
        if (ECB_entries_clean_up_get(pecb,i)) {
            SEP_PRINT_DEBUG("clean up set --- RegId --- %x\n", ECB_entries_reg_id(pecb,i));
            SYS_Write_MSR(ECB_entries_reg_id(pecb,i), 0LL);
        }
    } END_FOR_EACH_REG_ENTRY;

    return;
}

static VOID
corei7_Errata_Fix (
    void
)
{
    U64 mlc_event, rat_event, siu_event;
    U64 clr = 0;

    SEP_PRINT_DEBUG("Entered PMU Errata_Fix\n");
    mlc_event = 0x4300B5LL;
    rat_event = 0x4300D2LL;
    siu_event = 0x4300B1LL;

    SYS_Write_MSR(IA32_PEBS_ENABLE, clr);
    SYS_Write_MSR(IA32_PERF_GLOBAL_CTRL,clr);
    SYS_Write_MSR(0x186,mlc_event);
    SYS_Write_MSR(0xC1, clr);

    SYS_Write_MSR(0x187, rat_event);
    SYS_Write_MSR(0xC2, clr);

    SYS_Write_MSR(0x188, siu_event);
    SYS_Write_MSR(0xC3, clr);

    // this additional write seems to fix per counter issue
    // - some how an SIU event taken in the last counter in a group after
    // a group that has been sampling a SIU event renders the last counter useless
    // and it does not count
    SYS_Write_MSR(0x189, siu_event);
    SYS_Write_MSR(0xC4, clr);

    clr = 0xFLL;
    SYS_Write_MSR(IA32_PERF_GLOBAL_CTRL,clr);

    //hoping that by now it fixed the issue

    clr = 0x0;
    SYS_Write_MSR(IA32_PERF_GLOBAL_CTRL,clr);
    SYS_Write_MSR(0x186,clr);
    SYS_Write_MSR(0xc1,clr);
    SYS_Write_MSR(0x187,clr);
    SYS_Write_MSR(0xc2,clr);
    SYS_Write_MSR(0x188,clr);
    SYS_Write_MSR(0xC3, clr);
    SYS_Write_MSR(0x189,clr);
    SYS_Write_MSR(0xC4, clr);
    SEP_PRINT_DEBUG("Exited PMU Errata_Fix\n");

    return;
}

/*
 * core2_Check_Overflow_Htoff_Mode
 *
 *     Parameters
 *         INOUT masks
 *
 *     Returns
 *         Event mask of the overflowed registers.
 *
 *     Description
 *         Called by the data processing method to figure out
 *         which registers have overflowed.
 *         Does not include Nehalem and Bonnell specific code.
 *         Uncore also does not exist.
 *
 */
static void
core2_Check_Overflow_Htoff_Mode (
    DRV_MASKS    masks
)
{
    U32              index;
    U64              value               = 0;
    U64              overflow_status     = 0;
    U32              this_cpu            = CONTROL_THIS_CPU();
    BUFFER_DESC      bd                  = &cpu_buf[this_cpu];
    CPU_STATE        pcpu                = &pcb[this_cpu];
    ECB              pecb                = PMU_register_data[CPU_STATE_current_group(pcpu)];
    U64              overflow_status_clr = 0;
    DRV_EVENT_MASK_NODE event_flag;

    SEP_PRINT_DEBUG("core2_Check_Overflow_Htoff_Mode\n");

    // initialize masks 
    DRV_MASKS_masks_num(masks) = 0;

    overflow_status = SYS_Read_MSR(IA32_PERF_GLOBAL_STATUS);

    if (DRV_CONFIG_pebs_mode(pcfg)) {
        overflow_status = PEBS_Overflowed (this_cpu, overflow_status);
    }
    overflow_status_clr = overflow_status;
    SEP_PRINT_DEBUG("Overflow:  cpu: %d, status 0x%llx \n", this_cpu, overflow_status);
    index                        = 0;
    BUFFER_DESC_sample_count(bd) = 0;

    if (dispatch->check_overflow_gp_errata) {
        overflow_status = dispatch->check_overflow_gp_errata(pecb,  &overflow_status_clr);
    }

    FOR_EACH_DATA_REG(pecb, i) {
        if (ECB_entries_fixed_reg_get(pecb, i)) {
            index = ECB_entries_reg_id(pecb, i) - IA32_FIXED_CTR0 + 0x20;
        }
        else if (ECB_entries_is_compound_ctr_sub_bit_set(pecb, i)) {
             continue;
        }
        else if ((ECB_entries_is_compound_ctr_bit_set(pecb, i)) && !DRV_CONFIG_event_based_counts(pcfg)){
            index = COMPOUND_CTR_OVF_SHIFT;
        }
        else if (ECB_entries_is_gp_reg_get(pecb,i) && ECB_entries_reg_value(pecb,i) != 0) {
            index = ECB_entries_reg_id(pecb, i) - IA32_PMC0;
            if (ECB_entries_reg_id(pecb, i) >= IA32_PMC4 && 
                ECB_entries_reg_id(pecb, i) <= IA32_PMC7) {
                value = SYS_Read_MSR(ECB_entries_reg_id(pecb,i));
                if (value > 0 && value <= 0x100000000LL) {
                    overflow_status |= ((U64)1 << index);
                }
            } 
        }
        else {
            continue;
        }
        if (overflow_status & ((U64)1 << index)) {
            SEP_PRINT_DEBUG("Overflow:  cpu: %d, index %d\n", this_cpu, index);
            SEP_PRINT_DEBUG("register 0x%x --- val 0%llx\n",
                            ECB_entries_reg_id(pecb,i),
                            SYS_Read_MSR(ECB_entries_reg_id(pecb,i)));
            SYS_Write_MSR(ECB_entries_reg_id(pecb,i), ECB_entries_reg_value(pecb,i));

            DRV_EVENT_MASK_bitFields1(&event_flag) = (U8) 0;
            if (ECB_entries_precise_get(pecb, i)) {
                DRV_EVENT_MASK_precise(&event_flag) = 1;
            }
            if (ECB_entries_lbr_value_get(pecb, i)) {
                DRV_EVENT_MASK_lbr_capture(&event_flag) = 1;
            }

            if (DRV_MASKS_masks_num(masks) < MAX_OVERFLOW_EVENTS) {
                DRV_EVENT_MASK_bitFields1(DRV_MASKS_eventmasks(masks) + DRV_MASKS_masks_num(masks)) = DRV_EVENT_MASK_bitFields1(&event_flag);
                DRV_EVENT_MASK_event_idx(DRV_MASKS_eventmasks(masks) + DRV_MASKS_masks_num(masks)) = ECB_entries_event_id_index(pecb, i);
                DRV_MASKS_masks_num(masks)++;
            } 
            else {
                SEP_PRINT_ERROR("The array for event masks is full.\n");
            }

            SEP_PRINT_DEBUG("overflow -- 0x%llx, index 0x%llx\n", overflow_status, (U64)1 << index);
            SEP_PRINT_DEBUG("slot# %d, reg_id 0x%x, index %d\n",
                             i, ECB_entries_reg_id(pecb, i), index);
            if (ECB_entries_event_id_index(pecb, i) == CPU_STATE_trigger_event_num(pcpu)) {
                CPU_STATE_trigger_count(pcpu)--;
            }
        }
    } END_FOR_EACH_DATA_REG;

    // Reinitialize the global overflow control register
    SYS_Write_MSR(IA32_PERF_GLOBAL_OVF_CTRL, overflow_status_clr);

    SEP_PRINT_DEBUG("Check Overflow completed %d\n", this_cpu);
}

/*
 * core2_Read_Power
 *
 *     Parameters
 *         IN buffer - pointer to the buffer to write the data into
 *
 *     Returns
 *         NONE
 *
 *     Description
 *         Read all the PWR registers into the buffer provided and return.
 *
 */
static VOID
corei7_Read_Power (
    VOID   *buffer
)
{
    U32  i;
    U64 *pwr_buf = (U64 *)buffer;

    for (i = 0; i < PWR_num_entries(pwr); i++) {
        *pwr_buf = SYS_Read_MSR(PWR_entries_reg_id(pwr,i));
        pwr_buf++;
    }

    return;
}


/* ------------------------------------------------------------------------- */
/*!
 * @fn core2_Read_Counts(param, id)
 *
 * @param    param    The read thread node to process
 * @param    id       The event id for the which the sample is generated
 *
 * @return   None     No return needed
 *
 * @brief    Read CPU event based counts data and store into the buffer param;
 *           For the case of the trigger event, store the SAV value. 
 */
static VOID
core2_Read_Counts (
    PVOID  param,
    U32    id
)
{
    U64            *data;
    int             data_index;
    U32             this_cpu            = CONTROL_THIS_CPU();
    CPU_STATE       pcpu                = &pcb[this_cpu];
    U32             event_id            = 0;

    data       = (U64 *)param;
    data_index = 0;

    // Write GroupID
    data[data_index] = CPU_STATE_current_group(pcpu) + 1;
    // Increment the data index as the event id starts from zero
    data_index++;

    FOR_EACH_DATA_REG(pecb,i) {
        event_id = ECB_entries_event_id_index(pecb,i);
        if (event_id == id) {
            data[data_index + event_id] = ~(ECB_entries_reg_value(pecb,i) - 1) & 
                                           ECB_entries_max_bits(pecb,i);;
        }
        else {
            if (ECB_entries_is_compound_ctr_sub_bit_set(pecb,i)) {
                continue;
            }
            else {
                data[data_index + event_id] = SYS_Read_MSR(ECB_entries_reg_id(pecb,i));
                SYS_Write_MSR(ECB_entries_reg_id(pecb,i), 0LL);
            }
        }
    } END_FOR_EACH_DATA_REG;

    return;
}

/*
 * corei7_Check_Overflow_Errata
 *
 *     Parameters
 *         IN pecb:            The current event control block
 *         OUT overflow_status: current overflow mask
 *
 *     Returns
 *         Updated Event mask of the overflowed registers.
 *
 *     Description
 *         There is a bug where highly correlated precise events do
 *         not raise an indication on overflows in Core i7 and SNB.
 *
 */
static U64
corei7_Check_Overflow_Errata (
    ECB   pecb,
    U64   *overflow_status_clr
)
{
    U64 index = 0, value = 0, overflow_status = 0;
#if defined(MYDEBUG)
    U32 this_cpu = CONTROL_THIS_CPU();
#endif

    overflow_status = *overflow_status_clr;
    FOR_EACH_DATA_REG(pecb, i) {
        if (ECB_entries_reg_value(pecb, i) == 0) {
            continue;
        }
        if (ECB_entries_is_compound_ctr_sub_bit_set(pecb,i)) {
                continue;
        }
        if (ECB_entries_reg_id(pecb,i) == COMPOUND_PERF_CTR) {
            value = SYS_Read_MSR(ECB_entries_reg_id(pecb,i));
            if ((value == 0) || (value > 0LL && value <= 0x100000000LL)){
                overflow_status      |= ((U64)1 << COMPOUND_CTR_OVF_SHIFT);
            }
            break;
        }
        if (ECB_entries_is_gp_reg_get(pecb, i)) {
            index = ECB_entries_reg_id(pecb, i) - IA32_PMC0;
            value = SYS_Read_MSR(ECB_entries_reg_id(pecb,i));
            if (value > 0LL && value <= 0x100000000LL) {
                overflow_status      |= ((U64)1 << index);
                *overflow_status_clr |= ((U64)1 << index);
                // NOTE: system may hang if these debug statements are used
                SEP_PRINT_DEBUG("cpu %d counter 0x%x value 0x%llx\n",
                                this_cpu, ECB_entries_reg_id(pecb,i), value);                                   
            }
            continue;
        }
        if (ECB_entries_fixed_reg_get(pecb,i)) {
            index = ECB_entries_reg_id(pecb, i) - IA32_FIXED_CTR0 + 0x20;
            if (!(overflow_status & ((U64)1 << index))) {
                value = SYS_Read_MSR(ECB_entries_reg_id(pecb,i));
                if (ECB_entries_reg_id(pecb,i) == IA32_FIXED_CTR2) {
                    if (!(value > 0LL && value <= 0x1000000LL) && 
                        (*overflow_status_clr & ((U64)1 << index))) {
                        //Clear it only for overflow_status so that we do not create sample records 
                        //Please do not remove the check for MSR index
                        overflow_status = overflow_status & ~((U64)1 << index);
                        continue;
                    }
                }
                if (value > 0LL && value <= 0x100000000LL) {
                    overflow_status      |= ((U64)1 << index);
                    *overflow_status_clr |= ((U64)1 << index);
                    SEP_PRINT_DEBUG("cpu %d counter 0x%x value 0x%llx\n",
                                    this_cpu, ECB_entries_reg_id(pecb,i), value);
                }
            }
        } 
    } END_FOR_EACH_DATA_REG;

    return overflow_status;
}

/* ------------------------------------------------------------------------- */
/*!
 * @fn          U64 corei7_Read_Platform_Info
 *
 * @brief       Reads the MSR_PLATFORM_INFO register if present
 *
 * @param       void
 *
 * @return      value read from the register
 *
 * <I>Special Notes:</I>
 *              <NONE>
 */
static U64
corei7_Platform_Info (
    void
)
{
#define IA32_MSR_PLATFORM_INFO 0xCE
    U64 value = SYS_Read_MSR(IA32_MSR_PLATFORM_INFO);

    return value;
}

/*
 * Initialize the dispatch table
 */
DISPATCH_NODE  core2_dispatch =
{
    core2_Initialize,       // init
    core2_Destroy,          // fini
    core2_Write_PMU,        // write
    core2_Disable_PMU,      // freeze
    core2_Enable_PMU,       // restart
    core2_ReInit_Data,      // reinit
    core2_Read_PMU_Data,    // read
    core2_Check_Overflow,   // check for overflow
    core2_Swap_Group,
    core2_Read_LBRs,
    core2_Clean_Up,
    NULL,
    NULL,
    core2_Check_Overflow_Errata,
    core2_Read_Counts,
    NULL,
    NULL,                   // read_ro
    NULL                    // platform_info
};

DISPATCH_NODE  corei7_dispatch =
{
    core2_Initialize,       // init
    core2_Destroy,          // finis
    core2_Write_PMU,        // write
    core2_Disable_PMU,      // freeze
    core2_Enable_PMU,       // restart
    core2_ReInit_Data,      // reinit
    core2_Read_PMU_Data,    // read
    core2_Check_Overflow,   // check for overflow
    core2_Swap_Group,
    core2_Read_LBRs,
    core2_Clean_Up,
    corei7_Errata_Fix,
    corei7_Read_Power,
    NULL,
    core2_Read_Counts,
    corei7_Check_Overflow_Errata,
    NULL,                   // read_ro
    corei7_Platform_Info    // platform_info
};

DISPATCH_NODE  corei7_dispatch_htoff_mode =
{
    core2_Initialize,       // init
    core2_Destroy,          // fini
    core2_Write_PMU,        // write
    core2_Disable_PMU,      // freeze
    core2_Enable_PMU,       // restart
    core2_ReInit_Data,      // reinit
    core2_Read_PMU_Data,    // read
    core2_Check_Overflow_Htoff_Mode,   // check for overflow
    core2_Swap_Group,
    core2_Read_LBRs,
    core2_Clean_Up,
    corei7_Errata_Fix,
    corei7_Read_Power,
    NULL,
    core2_Read_Counts,
    corei7_Check_Overflow_Errata,
    NULL,                   // read_ro
    corei7_Platform_Info    // platform_info
};