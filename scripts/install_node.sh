#!/bin/bash

cecho(){  # source: https://stackoverflow.com/a/53463162/2886168
    RED="\033[0;31m"
    GREEN="\033[0;32m"
    YELLOW="\033[0;33m"
    NC="\033[0m" # No Color

    printf "${!1}${2} ${NC}\n"
}

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

print_help() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -h, --help       Show this help message and exit"
    echo "  -c               Install Python requirements from requirements.txt"
    echo "  -d               Stop and remove pingweave.service"
    echo ""
    echo "If no options are provided, the script will perform the following steps:"
    echo "  1. Check system prerequisites (systemd and Python >= 3.6)."
    echo "  2. Verify RDMA Core installation and install it if necessary."
    echo "  3. Navigate to the source directory, clean up, and build the project using make."
    echo "  4. Install and start the pingweave service."
}

# Report selected option or lack of options
if [[ -z "$1" ]]; then
    cecho "YELLOW" "No options provided. The script will proceed with the default operations."
else
    cecho "YELLOW" "Selected option: $1"
fi

# Handle -h or --help option
if [[ "$1" == "-h" || "$1" == "--help" ]]; then
    print_help
    exit 0
fi

# Handle -d option to stop and remove pingweave service immediately
if [[ "$1" == "-d" ]]; then
    cecho "YELLOW" "Stopping and removing pingweave service..."
    sudo systemctl stop pingweave.service || {
        cecho "RED" "Error: Failed to stop pingweave service."
        exit 1
    }
    sudo systemctl disable pingweave.service || {
        cecho "RED" "Error: Failed to disable pingweave service."
        exit 1
    }
    sudo rm /etc/systemd/system/pingweave.service || {
        cecho "RED" "Error: Failed to remove pingweave.service file."
        exit 1
    }
    sudo systemctl daemon-reload
    cecho "GREEN" "Pingweave service stopped and removed successfully."
    exit 0
fi

# Handle invalid options
if [[ -n "$1" && "$1" != "-c" ]]; then
    cecho "RED" "Error: Invalid option '$1'"
    print_help
    exit 1
fi

######## prerequisite ########
# (1) Check systemd
cecho "YELLOW" "Checking if systemd is running..."
if [[ "$(ps -p 1 -o comm=)" != "systemd" ]]; then
    cecho "RED" "Error: systemd is not running on this system."
    exit 1
fi
cecho "GREEN" "Systemd is running."

# (2) Check Python version (>= 3.6)
cecho "YELLOW" "Checking Python version..."
PYTHON_VERSION=$(python3 --version 2>/dev/null)

if [[ $? -ne 0 ]]; then
    cecho "RED" "Error: Python3 is not installed."
    exit 1
fi

VERSION_MAJOR=$(python3 -c "import sys; print(sys.version_info.major)")
VERSION_MINOR=$(python3 -c "import sys; print(sys.version_info.minor)")

if [[ $VERSION_MAJOR -ne 3 || $VERSION_MINOR -lt 6 ]]; then
    cecho "RED" "Error: Python version is less than 3.6. Found: $PYTHON_VERSION"
    exit 1
else
    cecho "GREEN" "Python version is 3.6 or higher: $PYTHON_VERSION"
fi

###### RDMA CORE ######
cecho "YELLOW" "Checking RDMA Core and related packages installation..."

if [[ -f /etc/redhat-release ]]; then
    # RHEL-based system
    PACKAGES=("rdma-core" "rdma-core-devel" "ibverbs-utils")
    for package in "${PACKAGES[@]}"; do
        if ! rpm -q $package &>/dev/null; then
            cecho "YELLOW" "$package is not installed. Installing..."
            sudo yum install -y $package || {
                cecho "RED" "Error: Failed to install $package."
                exit 1
            }
        else
            cecho "GREEN" "$package is already installed."
        fi
    done
elif [[ -f /etc/lsb-release || -f /etc/debian_version ]]; then
    # Ubuntu-based system
    PACKAGES=("rdma-core" "libibverbs-dev" "ibverbs-utils")
    for package in "${PACKAGES[@]}"; do
        if ! dpkg -l | grep -q $package; then
            cecho "YELLOW" "$package is not installed. Installing..."
            sudo apt update && sudo apt install -y $package || {
                cecho "RED" "Error: Failed to install $package."
                exit 1
            }
        else
            cecho "GREEN" "$package is already installed."
        fi
    done
else
    cecho "RED" "Unsupported OS. Please manually install RDMA Core and related packages."
    exit 1
fi

######### Make ########
cecho "YELLOW" "Navigating to source directory and cleaning up..."
cd "$SCRIPT_DIR/../src" || {
    cecho "RED" "Error: Directory '$SCRIPT_DIR/../src' not found or not accessible."
    exit 1
}

chmod +x "$SCRIPT_DIR/../src/clear.sh"
if [[ -x "./clear.sh" ]]; then
    bash ./clear.sh
    cecho "GREEN" "Cleanup completed."
else
    cecho "RED" "Error: './clear.sh' is not executable or not found."
    exit 1
fi

# make
cecho "YELLOW" "Running make..."
make || {
    cecho "RED" "Error: 'make' command failed."
    exit 1
}
cecho "GREEN" "Make completed successfully."

# check the result binary file
cecho "YELLOW" "Checking pingweave binary file exists..."
PINGWEAVE_BINARY_PATH="/pingweave/bin/pingweave"
if [ ! -f "$PINGWEAVE_BINARY_PATH" ]; then
  cecho "RED" "Error: pingweave directory must be located at /pingwewave. Current directory is $SCRIPT_DIR. "
  exit 1
fi
cecho "GREEN" "pingweave binary file -> $PINGWEAVE_BINARY_PATH"


######### INSTALL ########
cecho "YELLOW" "Installing pingweave service..."
# Register pingweave service to systemd
sudo cp "$SCRIPT_DIR/scripts/pingweave.service" /etc/systemd/system/ || {
    cecho "RED" "Error: Failed to copy pingweave.service to /etc/systemd/system/."
    exit 1
}

# Start pingweave service
sudo systemctl daemon-reload
sudo systemctl enable pingweave.service
sudo systemctl start pingweave.service || {
    cecho "RED" "Error: Failed to start pingweave service."
    exit 1
}

# Check service status
cecho "YELLOW" "Checking pingweave service status..."
sudo systemctl status pingweave.service || {
    cecho "RED" "Error: Failed to check pingweave service status."
    exit 1
}
cecho "GREEN" "Pingweave service installed and running successfully."

######### Additional Option ########
# Handle -c option for pip requirements
if [[ "$1" == "-c" ]]; then
    cecho "YELLOW" "Installing Python requirements from requirements.txt..."
    REQUIREMENTS_FILE="$SCRIPT_DIR/requirements.txt"
    if [[ -f "$REQUIREMENTS_FILE" ]]; then
        pip3 install -r "$REQUIREMENTS_FILE" || {
            cecho "RED" "Error: Failed to install Python requirements."
            exit 1
        }
        cecho "GREEN" "Python requirements installed successfully."
    else
        cecho "RED" "Error: requirements.txt not found at $REQUIREMENTS_FILE."
        exit 1
    fi
fi
