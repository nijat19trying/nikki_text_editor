#!/bin/bash

# Check for gcc
if ! command -v gcc &> /dev/null; then
    echo "gcc not found. Installing dependencies..."
    sudo apt update
    sudo apt install -y gcc make
fi

# Compile
gcc nikki.c -o nikki
if [ $? -ne 0 ]; then
    echo "Compilation failed. Check errors above."
    exit 1
fi

# Install to /usr/local/bin
sudo mv nikki /usr/local/bin/
if [ $? -eq 0 ]; then
    echo "Nikki installed successfully! Run with: nikki filename.txt"
else
    echo "Installation failed. Try running with sudo."
fi