#include <linux/vfio.h>
#include <linux/types.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>

#include <sys/fcntl.h>
#include <sys/mman.h>
#include <sys/eventfd.h>

#include <time.h>
     
//define mmap locations                    
#define	AXI_DMA_REGISTER_LOCATION          0xB0000000		//AXI DMA Register Address Map
#define	DESCRIPTOR_REGISTERS_SIZE          0xFFFF

#define	SG_DMA_DESCRIPTORS_WIDTH           0xFFFF
#define	MEMBLOCK_WIDTH                     0x3FFFFFF		//size of mem used by s2mm and mm2s
#define	BUFFER_BLOCK_WIDTH                 0x000010		    //size of memory block per descriptor in bytes
#define	NUM_OF_DESCRIPTORS                 0x1		        //number of descriptors for each direction

#define	HP0_DMA_BUFFER_MEM_ADDRESS         0x40000000
#define	HP0_MM2S_DMA_BASE_MEM_ADDRESS      (HP0_DMA_BUFFER_MEM_ADDRESS)
#define	HP0_MM2S_DMA_DESCRIPTORS_ADDRESS   (HP0_MM2S_DMA_BASE_MEM_ADDRESS)
#define	HP0_MM2S_SOURCE_MEM_ADDRESS        (HP0_MM2S_DMA_BASE_MEM_ADDRESS + SG_DMA_DESCRIPTORS_WIDTH + 1)

/*********************************************************************/
/*                   define all register locations                   */
/*               based on "LogiCORE IP Product Guide"                */
/*********************************************************************/
// MM2S CONTROL
#define MM2S_CONTROL_REGISTER       0x00    // MM2S_DMACR
#define MM2S_STATUS_REGISTER        0x04    // MM2S_DMASR
#define MM2S_CURDESC                0x08    // must align 0x40 addresses
#define MM2S_CURDESC_MSB            0x0C    // unused with 32bit addresses
#define MM2S_TAILDESC               0x10    // must align 0x40 addresses
#define MM2S_TAILDESC_MSB           0x14    // unused with 32bit addresses
#define SG_CTL                      0x2C    // CACHE CONTROL

int main(int argc, char **argv)
{
    unsigned int* axi_dma_register_mmap;
	unsigned int* mm2s_descriptor_register_mmap;
	unsigned int* source_mem_map;
	int controlregister_ok = 0,mm2s_status;
	uint32_t mm2s_current_descriptor_address;
	uint32_t mm2s_tail_descriptor_address;

	/*********************************************************************/
	/*               mmap the AXI DMA Register Address Map               */
	/*               the base address is defined in vivado               */
	/*                 by editing the offset address in                  */
	/*            address editor tab ("open block diagramm")             */
	/*********************************************************************/

	int dh = open("/dev/mem", O_RDWR | O_SYNC); 
	axi_dma_register_mmap = mmap(NULL, DESCRIPTOR_REGISTERS_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, dh, AXI_DMA_REGISTER_LOCATION);
	printf("AXI_DMA_REGISTER_LOCATION ok \n");

	mm2s_descriptor_register_mmap = mmap(NULL, DESCRIPTOR_REGISTERS_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, dh, HP0_MM2S_DMA_DESCRIPTORS_ADDRESS);
	printf("HP0_MM2S_DMA_DESCRIPTORS_ADDRESS ok \n");
	
	source_mem_map = mmap(NULL, BUFFER_BLOCK_WIDTH * NUM_OF_DESCRIPTORS, PROT_READ | PROT_WRITE, MAP_SHARED, dh, (off_t)(HP0_MM2S_SOURCE_MEM_ADDRESS));
	printf("HP0_MM2S_SOURCE_MEM_ADDRESS ok \n");

	int i;
	
	// fill mm2s-register memory with zeros
	for (i = 0; i < DESCRIPTOR_REGISTERS_SIZE; i++) {
		char *p = (char *)mm2s_descriptor_register_mmap;
		p[i] = 0x00000000;
	}
	printf("fill mm2s-register memory with zeros ok!\n");

	// fill source memory with a counter value
	for (i = 0; i < (BUFFER_BLOCK_WIDTH / 4) * NUM_OF_DESCRIPTORS; i++) {
		unsigned int *p = source_mem_map;
		p[i] = 0x00000000 + i; 
	}
	printf("fill source memory with a counter value ok!\n");

	/*********************************************************************/
	/*                 reset and halt all dma operations                 */
	/*********************************************************************/

	axi_dma_register_mmap[MM2S_CONTROL_REGISTER >> 2] =  0x4;
	axi_dma_register_mmap[MM2S_CONTROL_REGISTER >> 2] =  0x0;

	/*********************************************************************/
	/*           build mm2s and s2mm stream and control stream           */
	/* chains will be filled with next desc, buffer width and registers  */
	/*                         [0]: next descr                           */
	/*                         [1]: reserved                             */
	/*                         [2]: buffer addr                          */
	/*********************************************************************/

	mm2s_current_descriptor_address = HP0_MM2S_DMA_DESCRIPTORS_ADDRESS;                     // save current descriptor address

	mm2s_descriptor_register_mmap[0x0 >> 2] = HP0_MM2S_DMA_DESCRIPTORS_ADDRESS + 0x40;      // set next descriptor address
	mm2s_descriptor_register_mmap[0x8 >> 2] = HP0_MM2S_SOURCE_MEM_ADDRESS + 0x0;            // set target buffer address
	mm2s_descriptor_register_mmap[0x18 >> 2] = 0xC000010;                                   // set mm2s/s2mm buffer length to control register,16Byte

    mm2s_descriptor_register_mmap[0x20 >> 2] = 0x40800010;                                  //EOF=1,Type=1,BTT=0x10, APP0
    mm2s_descriptor_register_mmap[0x24 >> 2] = 0x80000000;                                  //APP1
    mm2s_descriptor_register_mmap[0x28 >> 2] = 0x00000000;                                  //写地址为0x80000000,2GB位置, APP2
    mm2s_descriptor_register_mmap[0x2C >> 2] = 0xFFFFFF00;                                  //高8位, APP3
    mm2s_descriptor_register_mmap[0x30 >> 2] = 0x11111111;                                  //APP4,没有用上

	mm2s_tail_descriptor_address = HP0_MM2S_DMA_DESCRIPTORS_ADDRESS;                        // save tail descriptor address, 只有一个描述符


	/*********************************************************************/
	/*                 set current descriptor addresses                  */
	/*           and start dma operations (S2MM_DMACR.RS = 1)            */
	/*********************************************************************/

	axi_dma_register_mmap[MM2S_CURDESC>>2] =  mm2s_current_descriptor_address;
	axi_dma_register_mmap[MM2S_CONTROL_REGISTER >> 2] =  0x1;

	/*********************************************************************/
	/*                          start transfer                           */
	/*                 (by setting the taildescriptors)                  */
	/*********************************************************************/
	axi_dma_register_mmap[MM2S_TAILDESC>>2] =  mm2s_tail_descriptor_address;

	/*********************************************************************/
	/*                 wait until all transfers finished                 */
	/*********************************************************************/

	while (!controlregister_ok)
    {
		mm2s_status = axi_dma_register_mmap[MM2S_STATUS_REGISTER >> 2];
		controlregister_ok = (mm2s_status & 0x00001000);
		printf("Memory-mapped to stream status (0x%08x@0x%02x):\n", mm2s_status, MM2S_STATUS_REGISTER);
		printf("MM2S_STATUS_REGISTER status register values:\n");
		if (mm2s_status & 0x00000001) printf(" halted"); else printf(" running");
		if (mm2s_status & 0x00000002) printf(" idle");
		if (mm2s_status & 0x00000008) printf(" SGIncld");
		if (mm2s_status & 0x00000010) printf(" DMAIntErr");
		if (mm2s_status & 0x00000020) printf(" DMASlvErr");
		if (mm2s_status & 0x00000040) printf(" DMADecErr");
		if (mm2s_status & 0x00000100) printf(" SGIntErr");
		if (mm2s_status & 0x00000200) printf(" SGSlvErr");
		if (mm2s_status & 0x00000400) printf(" SGDecErr");
		if (mm2s_status & 0x00001000) printf(" IOC_Irq");
		if (mm2s_status & 0x00002000) printf(" Dly_Irq");
		if (mm2s_status & 0x00004000) printf(" Err_Irq");
		printf("\n");
		printf("\n");
    }

	return 0;
}

