# S-IDNC Project

This project implements a Simple Instantly Decodable Network Coding (S-IDNC) system. It simulates the transmission of packets between a sender and multiple receivers, using network coding techniques to improve efficiency and reliability.

## Features

- **Packet Creation**: Generates a set of packets from source data.
- **Receiver Simulation**: Simulates multiple receivers with the ability to track received packets.
- **Network Coding**: Implements S-IDNC to encode and transmit packets efficiently.
- **Error Simulation**: Simulates packet loss during transmission.

## Project Structure

- `main.c`: The main entry point of the program, orchestrating the packet transmission and reception.
- `Sender.c/h`: Contains functions related to packet creation and encoding.
- `Receiver.c/h`: Contains functions and structures for simulating receivers.
- `Symbol.c/h`: Defines the data structures and functions for handling symbols and packets.

## Getting Started

### Prerequisites

- A C compiler (e.g., GCC)
- CMake for building the project

### Building the Project

1. Clone the repository:
   ```bash
   git clone https://github.com/yourusername/s-idnc.git
   cd s-idnc
   ```

2. Build the project using CMake:
   ```bash
   mkdir build
   cd build
   cmake ..
   make
   ```

### Running the Program

After building, you can run the program using:
```bash
./s-idnc
```


## Configuration

- **Number of Packets**: The number of packets per group is fixed at 32.
- **Number of Receivers**: The default is set to 10, but can be adjusted in `main.c`.

## Contributing

Contributions are welcome! Please fork the repository and submit a pull request for any improvements or bug fixes.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Acknowledgments

- This project is inspired by network coding research and aims to provide a simple implementation for educational purposes.