.syntax unified
.cpu cortex-m3
.thumb
.section .isr_vector,"a",%progbits
.global application_vectors
application_vectors:
    .word _stack_top, Reset_Handler
    .word Default_Handler, Default_Handler, Default_Handler, Default_Handler, Default_Handler
    .word 0xffffffff, 0xffffffff, 0xffffffff
    .word 0, Default_Handler, Default_Handler, 0xffffffff, Default_Handler, SysTick_Handler
    .rept 16
    .word Default_Handler
    .endr
    .word DMA0_Channel5_IRQHandler
    .word Default_Handler, Default_Handler
    .word USBD_HP_CAN0_TX_IRQHandler, USBD_LP_CAN0_RX0_IRQHandler
    .rept 17
    .word Default_Handler
    .endr
    .word USART1_IRQHandler
    .rept 21
    .word Default_Handler
    .endr

.weak SysTick_Handler
.thumb_set SysTick_Handler, Default_Handler
.weak USBD_HP_CAN0_TX_IRQHandler
.thumb_set USBD_HP_CAN0_TX_IRQHandler, Default_Handler
.weak USBD_LP_CAN0_RX0_IRQHandler
.thumb_set USBD_LP_CAN0_RX0_IRQHandler, Default_Handler
.weak DMA0_Channel5_IRQHandler
.thumb_set DMA0_Channel5_IRQHandler, Default_Handler
.weak USART1_IRQHandler
.thumb_set USART1_IRQHandler, Default_Handler

.section .text.Reset_Handler,"ax",%progbits
.thumb_func
.global Reset_Handler
Reset_Handler:
    cpsid i
    ldr r0, =_stack_top
    msr msp, r0                 /* before any C prologue or stack use */
    movs r0, #0
    msr control, r0
    msr basepri, r0
    isb
    ldr r1, =0xe000e010
    str r0, [r1]               /* SysTick CTRL */
    str r0, [r1, #4]           /* LOAD */
    str r0, [r1, #8]           /* VAL */
    ldr r1, =0xe000ed04
    ldr r2, =0x0a000000
    str r2, [r1]               /* clear pending SysTick and PendSV */
    ldr r1, =0xe000e180
    ldr r2, =0xe000e280
    mvn r3, #0
    movs r4, #8
1:  str r3, [r1], #4          /* NVIC ICER, all implemented banks */
    str r3, [r2], #4           /* NVIC ICPR */
    subs r4, #1
    bne 1b
    ldr r1, =0xe000ed08
    ldr r2, =application_vectors
    str r2, [r1]
    dsb
    isb
    ldr r1, =0x20004ff0
    str r0, [r1]               /* consume retained loader-entry marker */
    ldr r1, =_sidata
    ldr r2, =_sdata
    ldr r3, =_edata
2:  cmp r2, r3
    bhs 3f
    ldr r4, [r1], #4
    str r4, [r2], #4
    b 2b
3:  ldr r2, =_sbss
    ldr r3, =_ebss
4:  cmp r2, r3
    bhs 5f
    str r0, [r2], #4
    b 4b
5:  bl app_main
    b Default_Handler

.section .text.Default_Handler,"ax",%progbits
.thumb_func
.weak Default_Handler
Default_Handler:
    cpsid i
6:  wfi
    b 6b
