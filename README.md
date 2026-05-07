# ECE3073-ThursdayG5--1

- Nicholas Wong You Siang
    - 34897313
    - nwon0051@student.monash.edu
- Hon Yuen Neng
    - 34621075
    - yhon0037@student.monash.edu
- Ari Louis Jock
    - 34075720
    - alou0020@student.monash.edu


# Project Description 
This project is an embedded systems-based solution using the Intel FGPA DE10 platform with the NIOS II soft-core processor. It integrates hardware created in Quartus and software written in C (Eclipse) to control and interact with hardware components in real time.

# Features
- VGA display
- LED and HEX display
- Accelerometer
- Buzzer
- Interface with ESP32 via SPI
- Grooove AI via camera
- Multiprocessor system
- RTOS implementation

# Project User Manual Guide
1. Clone the Repository with
   ```shell
   ~$ git clone https://github.com/NicholasWongDaBest/ECE3073-ThursdayG5.git
   ```

3. Open Quartus Project
    - Launch Quartus Prime
    - Open the provided .qpf project file
    - Ensure all required files are included:
        - .bdf (Block Diagram File)
        - .qsys / Platform Designer system
        - Clock configuration files
        - Any HDL (.v, .vhd) files

4. Configure Platform Designer (NIOS II System)
    - Open Platform Designer (.qsys)
    - Verify that the system includes:
        - NIOS II processor
        - PIO (for switches, LEDs, HEX, buttons, speaker)
        - SPI / UART
        - SDRAM controller
        - Clock
        - On chip memory
    - Generate the system

5. Assign Pins
    - Open Pin Planner
    - Assign FPGA pins according to the DE10-Lite board:
        - Switches (SW)
        - LEDs
        - HEX displays
        - Push buttons
    - Save assignments

6. Compile the Project
    - Run:
        - Analysis & Synthesis
        - Full Compilation
    - Ensure there are no critical errors

7. Program the FPGA
    - Open Programmer
    - Load the .sof file
    - Select USB-Blaster
    - Click Start to program the board

8. Create NIOS II Software Project
    - Open NIOS II Software Build Tools (Eclipse)
    - Create:
        - New Application Project
        - Select your .sopcinfo file
    - Choose template (e.g., Hello World or blank)

9. Build and Run
    - Copy & Paste / Write your C code to:
        - Control HEX display
        - Read switches
        - Control LEDs
        - VGA 
        - SPI
        - Accelerometer
        - ETC.
    - Build project
    - Run on hardware

10. Basic Functionality
    - Ensure all the peripherals or base functions work
    - All peripherals should work using polling (interrupt optional)

11. Testing
    - Verify all hardware components respond correctly
    - Ensure no compilation or runtime errors
    - Demonstrate stable operation for all switches and displays
