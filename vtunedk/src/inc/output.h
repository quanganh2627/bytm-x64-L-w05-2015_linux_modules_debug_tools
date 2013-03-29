/*
    Copyright (C) 2005-2012 Intel Corporation.  All Rights Reserved.
 
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
*/

#ifndef _OUTPUT_H_
#define _OUTPUT_H_

/* 
 * Initial allocation 
 * Size of buffer     = 512KB (2^19)
 * number of buffers  = 2
 * The max size of the buffer cannot exceed 1<<22 i.e. 4MB
 */
#define OUTPUT_SMALL_BUFFER        (1<<15)
#define OUTPUT_LARGE_BUFFER        (1<<19)
#define OUTPUT_MEMORY_THRESHOLD    0x8000000

extern  U32                   output_buffer_size;
#define OUTPUT_BUFFER_SIZE    output_buffer_size
#define OUTPUT_NUM_BUFFERS    2

/*
 *  Data type declarations and accessors macros
 */
typedef struct {
    spinlock_t    buffer_lock;
    U32           remaining_buffer_size;
    U32           current_buffer;
    U32           next_buffer[OUTPUT_NUM_BUFFERS];
    U32           buffer_full[OUTPUT_NUM_BUFFERS];
    U8           *buffer[OUTPUT_NUM_BUFFERS];
} OUTPUT_NODE, *OUTPUT;

#define OUTPUT_buffer_lock(x)            (x)->buffer_lock
#define OUTPUT_remaining_buffer_size(x)  (x)->remaining_buffer_size
#define OUTPUT_total_buffer_size(x)      OUTPUT_BUFFER_SIZE
#define OUTPUT_buffer(x,y)               (x)->buffer[(y)]
#define OUTPUT_buffer_full(x,y)          (x)->buffer_full[(y)]
#define OUTPUT_current_buffer(x)         (x)->current_buffer

/*
 *  Add an array of control buffer for per-cpu 
 */
typedef struct {
    wait_queue_head_t queue;
    OUTPUT_NODE      outbuf;
    U32              sample_count;
} BUFFER_DESC_NODE, *BUFFER_DESC;

#define BUFFER_DESC_queue(a)          (a)->queue
#define BUFFER_DESC_outbuf(a)         (a)->outbuf
#define BUFFER_DESC_sample_count(a)   (a)->sample_count

extern BUFFER_DESC   cpu_buf;  // actually an array of BUFFER_DESC_NODE
extern BUFFER_DESC   module_buf;

/*
 *  Interface Functions
 */

extern int       OUTPUT_Module_Fill (PVOID data, U16 size);
extern OS_STATUS OUTPUT_Initialize (char *buffer, unsigned long len);
extern int       OUTPUT_Destroy (VOID);
extern int       OUTPUT_Flush (VOID);
extern ssize_t   OUTPUT_Module_Read (struct file *filp, char *buf, size_t count, loff_t *f_pos);
extern ssize_t   OUTPUT_Sample_Read (struct file *filp, char *buf, size_t count, loff_t *f_pos);
extern void*     OUTPUT_Reserve_Buffer_Space (BUFFER_DESC  bd, U32 size);

#endif 
