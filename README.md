<p align="center">
    <img width="30%" src="./Preview/logo-high-res.png" alt="brand logo">
</p>

# JSONWAVE

JSONWAVE is a powerful JSON library written in C that provides a wide range of tools for handling JSON data.

## Table of Contents
- [Introduction](#introduction)
- [How It Works](#how-it-works)
- [Features](#features)
- [Disclaimer](#disclaimer)
- [Installation](#installation)
- [Demo](#demo)
- [Tools](#tools)
- [License](#license)

## Introduction

JSONWAVE is the result of an internship at Telnet Holding , where it was created to simplify the process of handling JSON data. It serves as a bridge between the client and server, taking raw JSON data from the client-side, parsing it, and forwarding it to the server for further processing.

## How It Works

  ![Image 1](./Preview/jsonwave.gif)

JSONWAVE serves as a middleware between the client and server. It takes raw JSON data from the client-side, parses it, and forwards it to the server for further processing. This simplifies the handling of JSON data and ensures seamless communication between the client and server.

## Features
- Lightweight (only 2 files)
- Very Simple
- Low footprint
- Perfect for low memory systems
- C89 compatible
- Test suites

## Disclaimer
This project has been thoroughly tested on both Windows and Linux using different compilers, including:
- GCC (Linux & Windows)
- Microsoft Visual Studio

## Installation
Clone the repository:
```
git clone https://github.com/Sami-Souissi/JSONWAVE
```

Compile and run tests using the following command:
```
gcc -o test_json -I.. test_json.c ./json.c -lm
```

Usage:
```
./test_json <json_file>
```

## Demo
JSONWAVE includes a demo file titled `test_json` that showcases all the available tools in a user-friendly menu format.



## Tools
JSONWAVE provides a variety of tools to work with JSON data, including:
- Parser
- Getter
- Setter
- Export
- Display
- Syntax Error Detection
- Update

## Contributing

We welcome contributions to improve JSONWave. If you'd like to contribute, please follow these guidelines:

1. Fork the repository.

2. Create a new branch for your feature or fix.

3. Make your changes and commit them.

4. Push your changes to your fork.

5. Submit a pull request to the main repository.

## License

This project is licensed under the [MIT License](LICENSE.md) - see the [LICENSE.md](LICENSE.md) file for details.

