# Universal-Gateway-Device
Project Overview

The Universal Industrial Gateway Device is an IoT-based solution designed to collect real-time machine data through digital input signals. It continuously captures machine status, operating signals, and interlocks, and transfers the acquired data to a central database for monitoring and analysis.

The system uses an ESP32-based custom PCB as the field-level controller, with RS485 for industrial data acquisition and Ethernet for network connectivity. It provides 5 digital inputs, 2 digital outputs, relay integration, and optocoupler-based signal isolation. In the second phase, a Raspberry Pi is integrated through UART to handle higher-level processing and host the HMI for local visualization and operator interaction.

The project is developed in two phases: Phase 1 focuses on building a reliable gateway for machine data acquisition and database transmission, while Phase 2 adds display and HMI capabilities. The overall goal is to provide universal machine connectivity, reliable data logging, real-time monitoring, scalable deployment, and seamless integration with the Operator Buddy system, including functions such as downtime calculation, work-order tracking, and downtime reason logging.
