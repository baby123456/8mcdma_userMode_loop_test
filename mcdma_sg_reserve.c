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
#define AXI_DMA_REGISTER_LOCATION          0xB0000000           //AXI DMA Register Address Map
#define AXI_DMA1_REGISTER_LOCATION         0xB0001000           //AXI DMA1 Register Address Map

#define DESCRIPTOR_REGISTERS_SIZE          0xFFFF

#define SG_DMA_DESCRIPTORS_WIDTH           0xFFFF
#define MEMBLOCK_WIDTH                     0x3FFFFFF            //size of mem used by s2mm and mm2s,48MB
#define BUFFER_BLOCK_WIDTH                 0x4000               //size of memory block per descriptor in bytes
#define NUM_OF_DESCRIPTORS                 0x1                  //number of descriptors for each direction

#define HP0_DMA_BUFFER_MEM_ADDRESS         0x40000000
#define HP0_MM2S_DMA_BASE_MEM_ADDRESS      (HP0_DMA_BUFFER_MEM_ADDRESS)
#define HP0_MM2S_DMA_DESCRIPTORS_ADDRESS   (HP0_MM2S_DMA_BASE_MEM_ADDRESS)
#define HP0_MM2S_SOURCE_MEM_ADDRESS        (HP0_MM2S_DMA_BASE_MEM_ADDRESS + SG_DMA_DESCRIPTORS_WIDTH + 1)

#define HP0_MM2S_DMA1_BASE_MEM_ADDRESS     (HP0_DMA_BUFFER_MEM_ADDRESS + MEMBLOCK_WIDTH + 1)
#define HP0_MM2S_DMA1_DESCRIPTORS_ADDRESS  (HP0_MM2S_DMA1_BASE_MEM_ADDRESS)
#define HP0_MM2S_TARGET_MEM_ADDRESS        (HP0_MM2S_DMA1_BASE_MEM_ADDRESS + SG_DMA_DESCRIPTORS_WIDTH + 1)


/*********************************************************************/
/*                   define all register locations                   */
/*               based on "LogiCORE IP Product Guide"                */
/*********************************************************************/
// MM2S CONTROL
#define MM2S_DMACR			    0x0000					    //MM2S通用控制寄存器
#define MM2S_DMACR_RESET		BIT(2)					    //MM2S通用复位控制位
#define MM2S_DMACR_RUNSTOP		BIT(0)					    //MM2S通用启停控制位

#define MM2S_DMASR			    0x0004					    //MM2S通用状态寄存器
#define MM2S_DMASR_HALTED	    BIT(0)						//MM2S停止状态位
#define MM2S_DMASR_IDLE		    BIT(1)						//MM2S空闲状态位

#define MM2S_CHEN_OFFSET		0x0008					    //MM2S特定通道启用关闭寄存器

#define MM2S_CH1CR                  0x040
#define MM2S_CH1SR                  0x044
#define MM2S_CURDESC                0x048                   // must align 0x40 addresses
#define MM2S_CURDESC_MSB            0x04c                   // unused with 32bit addresses
#define MM2S_TAILDESC               0x050                   // must align 0x40 addresses
#define MM2S_TAILDESC_MSB           0x054                   // unused with 32bit addresses
#define MM2S_PKTCNT_STAT            0x058

int main(int argc, char **argv)
{
        unsigned int* axi_dma_register_mmap;
        unsigned int* axi_dma1_register_mmap;
        unsigned int* mm2s_descriptor_register_mmap;
        unsigned int* mm2s_DMA1_descriptor_register_mmap;
        unsigned int* source_mem_map;
        unsigned int* dest_mem_map;
        int controlregister_ok = 0,mm2s_status;
        uint32_t mm2s_current_descriptor_address;
        uint32_t mm2s_tail_descriptor_address;
        uint32_t DMA1_mm2s_current_descriptor_address;
        uint32_t DMA1_mm2s_tail_descriptor_address;


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

        mm2s_DMA1_descriptor_register_mmap = mmap(NULL, DESCRIPTOR_REGISTERS_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, dh, HP0_MM2S_DMA1_DESCRIPTORS_ADDRESS);
        printf("HP0_MM2S_DMA1_DESCRIPTORS_ADDRESS ok \n");

        dest_mem_map = mmap(NULL, BUFFER_BLOCK_WIDTH * NUM_OF_DESCRIPTORS, PROT_READ | PROT_WRITE, MAP_SHARED, dh, (off_t)(HP0_MM2S_TARGET_MEM_ADDRESS));
        printf("HP0_S2MM_TARGET_MEM_ADDRESS ok \n");

        axi_dma1_register_mmap = mmap(NULL, DESCRIPTOR_REGISTERS_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, dh, AXI_DMA1_REGISTER_LOCATION);
        printf("AXI_DMA1_REGISTER_LOCATION ok \n");

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
                p[i] = 0x00000001 + i;
        }
        printf("fill source memory with a counter value ok!\n");

        // fill mm2s-DMA1 register memory with zeros
        for (i = 0; i < DESCRIPTOR_REGISTERS_SIZE; i++) {
                char *p = (char *)mm2s_DMA1_descriptor_register_mmap;
                p[i] = 0x00000000;
        }
        printf("fill mm2s-DMA1 register memory with zeros ok!\n");

        // fill target memory with zeros
        for (i = 0; i < (BUFFER_BLOCK_WIDTH / 4) * NUM_OF_DESCRIPTORS; i++) {
                unsigned int *p = dest_mem_map;
                p[i] = 0x0;
        }
        printf("fill target memory with zeros!\n");

        /*********************************************************************/
        /*                 reset and halt all dma operations                 */
        /*********************************************************************/
        axi_dma_register_mmap[MM2S_DMACR >> 2] =  0x4;
        axi_dma_register_mmap[MM2S_DMACR >> 2] =  0x0;

        /*********************************************************************/
        /*           build mm2s stream and control stream                    */
        /* chains will be filled with next desc, buffer width and registers  */
        /*                         [0]: next descr                           */
        /*                         [1]: reserved                             */
        /*                         [2]: buffer addr                          */
        /*********************************************************************/

        mm2s_current_descriptor_address = HP0_MM2S_DMA_DESCRIPTORS_ADDRESS;                     // save current descriptor address

        mm2s_descriptor_register_mmap[0x0 >> 2] = HP0_MM2S_DMA_DESCRIPTORS_ADDRESS + 0x40;      // set next descriptor address
        mm2s_descriptor_register_mmap[0x4 >> 2] = 0x0;                                          // set next descriptor address MSB                
        mm2s_descriptor_register_mmap[0x8 >> 2] = HP0_MM2S_SOURCE_MEM_ADDRESS + 0x0;            // set target buffer address
        mm2s_descriptor_register_mmap[0xc >> 2] = 0x0;
        mm2s_descriptor_register_mmap[0x14 >> 2] = 0xC0004000;                                   // set mm2s/s2mm buffer length to control register,16Byte


        mm2s_descriptor_register_mmap[0x20 >> 2] = 0x40804000;                                  //EOF=1,Type=1,BTT=0x10, APP0
        mm2s_descriptor_register_mmap[0x24 >> 2] = 0x80000000;                                  //APP1
        mm2s_descriptor_register_mmap[0x28 >> 2] = 0x00000000;                                  //写地址为0x80000000,2GB位置, APP2
        mm2s_descriptor_register_mmap[0x2C >> 2] = 0xFFFFFF00;                                  //高8位, APP3
        mm2s_descriptor_register_mmap[0x30 >> 2] = 0x11111111;                                  //APP4,没有用▒

        mm2s_tail_descriptor_address = HP0_MM2S_DMA_DESCRIPTORS_ADDRESS;                        // save tail descriptor address, 只有一个描述符


        /*********************************************************************/
        /*                 set current descriptor addresses                  */
        /*           and start dma operations (MM2S_DMACR.RS = 1)            */
        /*********************************************************************/
        axi_dma_register_mmap[MM2S_CHEN_OFFSET>>2] = 0x1;

        axi_dma_register_mmap[MM2S_CURDESC>>2] =  mm2s_current_descriptor_address;
        axi_dma_register_mmap[MM2S_CURDESC_MSB >>2] =  0;

        //Start the fetch of BDs for channel0.
        axi_dma_register_mmap[MM2S_CH1CR >> 2] =  0x1;
        axi_dma_register_mmap[MM2S_DMACR >> 2] =  0x1;

        //set Interrupt Threshold=1
        mm2s_status = axi_dma_register_mmap[MM2S_CH1CR >> 2];
        axi_dma_register_mmap[MM2S_CH1CR >> 2] =  mm2s_status & 0xFF01FFFF;
        //Interrupt on Complete Interrupt Enable
        mm2s_status = axi_dma_register_mmap[MM2S_CH1CR >> 2];
        axi_dma_register_mmap[MM2S_CH1CR >> 2] =  mm2s_status | 0x20;

        /*********************************************************************/
        /*                          start transfer                           */
        /*                 (by setting the taildescriptors)                  */
        /*********************************************************************/
        axi_dma_register_mmap[MM2S_TAILDESC>>2] =  mm2s_tail_descriptor_address;
        axi_dma_register_mmap[MM2S_TAILDESC_MSB>>2] =  0;

        /*********************************************************************/
        /*                 wait until all transfers finished                 */
        /*********************************************************************/

        while (!controlregister_ok)
        {
                    mm2s_status = axi_dma_register_mmap[MM2S_CH1SR >> 2];
                    controlregister_ok = (mm2s_status & 0x00000020);
                    printf("Memory-mapped to stream status (0x%08x@0x%02x):\n", mm2s_status, MM2S_CH1SR);
                    printf("MM2S_STATUS_REGISTER status register values:\n");
                    if (mm2s_status & 0x00000001) printf(" idle");
                    if (mm2s_status & 0x00000008) printf(" Err_on_other_Irq");
                    if (mm2s_status & 0x00000020) printf(" IOC_Irq");
                    if (mm2s_status & 0x00000040) printf(" Dly_Irq");
                    if (mm2s_status & 0x00000080) printf(" Err_Irq");
                    printf("\n");
                    printf("\n");
        }


        /*********************************************************************/
        /*                 reset and halt all dma1 operations                 */
        /*********************************************************************/

        axi_dma1_register_mmap[MM2S_DMACR >> 2] =  0x4;
        axi_dma1_register_mmap[MM2S_DMACR >> 2] =  0x0;

        /*********************************************************************/
        /*           build mm2s and s2mm stream and control stream           */
        /* chains will be filled with next desc, buffer width and registers  */
        /*                         [0]: next descr                           */
        /*                         [1]: reserved                             */
        /*                         [2]: buffer addr                          */
        /*********************************************************************/

        DMA1_mm2s_current_descriptor_address = HP0_MM2S_DMA1_DESCRIPTORS_ADDRESS;                     // save current descriptor address

        mm2s_DMA1_descriptor_register_mmap[0x0 >> 2] = HP0_MM2S_DMA1_DESCRIPTORS_ADDRESS + 0x40;      // set next descriptor address
        mm2s_DMA1_descriptor_register_mmap[0x4 >> 2] = 0x0;
        mm2s_DMA1_descriptor_register_mmap[0x8 >> 2] = 0x80000000;            // set target buffer address
        mm2s_DMA1_descriptor_register_mmap[0xc >> 2] = 0x0;
        mm2s_DMA1_descriptor_register_mmap[0x18 >> 2] = 0xC0004000;                                   // set mm2s/s2mm buffer length to control register,16Byte


        mm2s_DMA1_descriptor_register_mmap[0x20 >> 2] = 0x40804000;                                  //EOF=1,Type=1,BTT=0x10, APP0
        mm2s_DMA1_descriptor_register_mmap[0x24 >> 2] = HP0_MM2S_TARGET_MEM_ADDRESS + 0x0;                                  //APP1
        mm2s_DMA1_descriptor_register_mmap[0x28 >> 2] = 0x00000000;                                  //写地址为0x80000000,2GB位置, APP2
        mm2s_DMA1_descriptor_register_mmap[0x2C >> 2] = 0xFFFFFF00;                                  //高8位, APP3
        mm2s_DMA1_descriptor_register_mmap[0x30 >> 2] = 0x11111111;                                  //APP4,没有用▒

        DMA1_mm2s_tail_descriptor_address = HP0_MM2S_DMA1_DESCRIPTORS_ADDRESS;                        // save tail descriptor address, 只有一个描述符


        /*********************************************************************/
        /*                 set current descriptor addresses                  */
        /*           and start dma operations (S2MM_DMACR.RS = 1)            */
        /*********************************************************************/

        axi_dma1_register_mmap[MM2S_CURDESC>>2] =  DMA1_mm2s_current_descriptor_address;
        axi_dma1_register_mmap[MM2S_CURDESC_MSB >>2] =  0;

        //Start the fetch of BDs for channel0.
        axi_dma1_register_mmap[MM2S_CH1CR >> 2] =  0x1;
        axi_dma1_register_mmap[MM2S_DMACR >> 2] =  0x1;

        //set Interrupt Threshold=1
        mm2s_status = axi_dma1_register_mmap[MM2S_CH1CR >> 2];
        axi_dma1_register_mmap[MM2S_CH1CR >> 2] =  mm2s_status & 0xFF01FFFF;
        //Interrupt on Complete Interrupt Enable
        mm2s_status = axi_dma1_register_mmap[MM2S_CH1CR >> 2];
        axi_dma1_register_mmap[MM2S_CH1CR >> 2] =  mm2s_status | 0x20;

        /*********************************************************************/
        /*                          start transfer                           */
        /*                 (by setting the taildescriptors)                  */
        /*********************************************************************/
        axi_dma1_register_mmap[MM2S_TAILDESC>>2] =  DMA1_mm2s_tail_descriptor_address;
        axi_dma1_register_mmap[MM2S_TAILDESC_MSB>>2] =  0;

        /*********************************************************************/
        /*                 wait until all transfers finished                 */
        /*********************************************************************/

       while (!controlregister_ok)
        {
                    mm2s_status = axi_dma_register_mmap[MM2S_CH1SR >> 2];
                    controlregister_ok = (mm2s_status & 0x00000020);
                    printf("Memory-mapped to stream status (0x%08x@0x%02x):\n", mm2s_status, MM2S_CH1SR);
                    printf("MM2S_STATUS_REGISTER status register values:\n");
                    if (mm2s_status & 0x00000001) printf(" idle");
                    if (mm2s_status & 0x00000008) printf(" Err_on_other_Irq");
                    if (mm2s_status & 0x00000020) printf(" IOC_Irq");
                    if (mm2s_status & 0x00000040) printf(" Dly_Irq");
                    if (mm2s_status & 0x00000080) printf(" Err_Irq");
                    printf("\n");
                    printf("\n");
        }


        int c , flag = 0;
        int fail_count = 0 , success_count = 0;
        unsigned int *src_ptr = source_mem_map;
        unsigned int *dst_ptr = dest_mem_map;
        for(c = 0; c < (BUFFER_BLOCK_WIDTH / 4) * NUM_OF_DESCRIPTORS; c++) {
                if(src_ptr[c] != dst_ptr[c]) {

                        if(!flag) {
                                flag = 1;
                                //printf("test failed! - %d - 0x%x - 0x%x\n", c, src_ptr[c], dst_ptr[c]);
                        }
                        printf("test failed! - %d - 0x%x - 0x%x\n", c, src_ptr[c], dst_ptr[c]);
                        fail_count++;
                }
                else{
                        if(flag) {
                                flag = 0;
                                //printf("test success! - %d - 0x%x - 0x%x\n", c, 4*src_ptr[c], dst_ptr[c]);
                        }
                        printf("test success! - %d - 0x%x - 0x%x\n", c, src_ptr[c], dst_ptr[c]);
                        success_count++;
                }
        }
        printf("fail count : %d\n",fail_count);
        printf("success count : %d\n",success_count);

        return 0;
}
