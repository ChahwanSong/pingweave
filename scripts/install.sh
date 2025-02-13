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
    echo "  -d               Stop and remove pingweave.service"
    echo "  -c               Follow the installation steps for controller node"
    echo "  -p               Copy scripts/pip.conf to ~/.pip"
    echo ""
    echo "If no options are provided, the script will perform the following steps:"
    echo "  1. Check system prerequisites (systemd and Python >= 3.6), NTP, systemd, etc."
    echo "  2. Install python packages in /scripts/requirements_*.txt."
    echo "  3. Verify RDMA Core installation and install it if necessary."
    echo "  4. Navigate to the source directory, clean up, and build the project using make."
    echo "  5. Install and start the pingweave service."
}

# Report selected option or lack of options
if [[ -z "$1" ]]; then
    cecho "YELLOW" "No options provided. The script will proceed with the default operations."
else
    cecho "YELLOW" "Selected option: $1 $2"
fi

# Handle -h or --help option
if [[ "$1" == "-h" || "$1" == "--help" ]]; then
    print_help
    exit 0
fi

# Check if running as root
if [ "$EUID" -ne 0 ]; then
    cecho "RED" "Error: This script must be run as root."
    exit 1
fi

# Clean up shared memory in /dev/shm
if [[ ! -d /dev/shm ]]; then
    cecho "RED" "Error: /dev/shm does not exist or is not a directory." >&2
    exit 1
fi
if [[ ! -w /dev/shm ]]; then
    cecho "RED" "Error: /dev/shm is not writable. You may need elevated privileges." >&2
    exit 1
fi

# Collect files matching the specified prefixes.
shopt -s nullglob
shm_files=(/dev/shm/{tcp,udp,roce,ib}_*)
for shm_file in "${shm_files[@]}"; do
    if [ -f "$shm_file" ]; then
        if rm -f "$shm_file"; then
            cecho "GREEN" "Removed: $shm_file"
        else
            cecho "RED" "Error: Failed to remove $shm_file"
            exit 1;
        fi
    else
        cecho "YELLOW" "Skipping (not a regular shm_file): $shm_file"
    fi
done


# Handle -d option to stop and remove pingweave service immediately
if [[ " $@ " == *" -d "* ]]; then
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


# Check if -p is present in any argument
if [[ " $@ " == *" -p "* ]]; then
    cecho "YELLOW" "Copying pip.conf file to ~/.pip directory..."
    mkdir -p "$HOME/.pip"
    cp "$SCRIPT_DIR/pip.conf" "$HOME/.pip"
    cecho "GREEN" "Copying pip.conf to $HOME/.pip directory is successful"
fi


######## prerequisite ########
# (1) Check systemd
cecho "YELLOW" "Checking if systemd is running..."
if [[ "$(ps -p 1 -o comm=)" != "systemd" ]]; then
    cecho "RED" "Error: systemd is not running on this system."
    exit 1
fi
cecho "GREEN" "Systemd is running."

# (2) chronyd (NTP) systemd
CHRONYD_SERVICE="chronyd.service"
cecho "YELLOW" "Checking if $CHRONYD_SERVICE (NTP) is running..."
if systemctl is-active --quiet "$CHRONYD_SERVICE"; then
    cecho "GREEN" "$CHRONYD_SERVICE is already running. Just restart it."
    systemctl restart "$CHRONYD_SERVICE"
else
    cecho "YELLOW" "$CHRONYD_SERVICE is not running. Starting and enabling it..."
    # 서비스 시작
    systemctl start "$CHRONYD_SERVICE"
    if [ $? -eq 0 ]; then
        cecho "GREEN" "$CHRONYD_SERVICE started successfully."
    else
        cecho "RED" "Failed to start $CHRONYD_SERVICE." >&2
        exit 1
    fi

    # 서비스 활성화
    systemctl enable "$CHRONYD_SERVICE"
    if [ $? -eq 0 ]; then
        cecho "GREEN" "$CHRONYD_SERVICE is now enabled."
    else
        cecho "RED" "Failed to enable $CHRONYD_SERVICE." >&2
        exit 1
    fi
fi


# (3) Check Python version (>= 3.6 for agent and 3.7 for controller)
cecho "YELLOW" "Checking Python version..."
PYTHON_VERSION=$(python3 --version 2>/dev/null)

if [[ $? -ne 0 ]]; then
    cecho "RED" "Error: Python3 is not installed."
    exit 1
fi

VERSION_MAJOR=$(python3 -c "import sys; print(sys.version_info.major)")
VERSION_MINOR=$(python3 -c "import sys; print(sys.version_info.minor)")

if [[ " $@ " == *" -c "* ]]; then
    if [[ $VERSION_MAJOR -ne 3 || $VERSION_MINOR -lt 7 ]]; then
        cecho "RED" "Error: Controller's python version is less than 3.7. Found: $PYTHON_VERSION"
        exit 1
    else
        cecho "GREEN" "Python version is 3.7 or higher: $PYTHON_VERSION"
    fi
else
    if [[ $VERSION_MAJOR -ne 3 || $VERSION_MINOR -lt 6 ]]; then
        cecho "RED" "Error: Agent's python version is less than 3.6. Found: $PYTHON_VERSION"
        exit 1
    else
        cecho "GREEN" "Python version is 3.6 or higher: $PYTHON_VERSION"
    fi
fi

# (4) check that redis package is installed and configured ---
if [[ " $@ " == *" -c "* ]]; then
    cecho "YELLOW" "Checking if redis package is installed..."

    # Check if redis package is installed depending on the OS
    if [[ -f /etc/redhat-release ]]; then
        # RHEL-based systems
        if ! rpm -q redis &>/dev/null; then
            cecho "RED" "Error: redis package is not installed. Please install redis and try again."
            exit 1
        fi
    elif [[ -f /etc/lsb-release || -f /etc/debian_version ]]; then
        # Ubuntu-based systems (the package name might be redis or redis-server)
        if ! dpkg -l | grep -qw redis; then
            cecho "RED" "Error: redis package is not installed. Please install redis and try again."
            exit 1
        fi
    else
        cecho "RED" "Unsupported OS for redis package check."
        exit 1
    fi
    cecho "GREEN" "redis package is installed."

    # --- Now check that redis is configured for unix domain socket communication ---
    cecho "YELLOW" "Checking redis configuration for unix domain socket communication..."

    # Determine redis config file location (try common paths)
    if [ -f /etc/redis/redis.conf ]; then
        REDIS_CONF="/etc/redis/redis.conf"
    elif [ -f /etc/redis.conf ]; then
        REDIS_CONF="/etc/redis.conf"
    else
        cecho "RED" "Error: Could not locate the redis configuration file."
        echo "$(
cat << 'EOF'
To enable unix domain socket, you must modify a redis configuration file (typically located at /etc/redis/redis.conf or /etc/redis.conf) and restart the redis.service:

unixsocket /var/run/redis/redis-server.sock # specify the path to unix socket
unixsocketperm 700 # usually 700 or 770
EOF
)"
        exit 1
    fi

    # Initialize a flag to detect missing configuration
    missing_config=false

    # Check for the "unixsocket" configuration
    if ! grep -E "^[[:space:]]*unixsocket[[:space:]]+/var/run/redis/redis-server.sock" "$REDIS_CONF" | grep -qv "^[[:space:]]*#"; then
        cecho "RED" "Error: redis configuration missing: 'unixsocket /var/run/redis/redis-server.sock'"
        missing_config=true
    fi

    # Check for the "unixsocketperm" configuration
    if ! grep -E "^[[:space:]]*unixsocketperm[[:space:]]+700" "$REDIS_CONF" | grep -qv "^[[:space:]]*#"; then
        cecho "RED" "Error: redis configuration missing: 'unixsocketperm 700'"
        missing_config=true
    fi

    # If any configuration is missing, print the instruction message and exit
    if $missing_config; then
        cecho "RED" "The redis configuration for unix domain socket is not applied correctly."
        echo "$(
cat << 'EOF'
To enable unix domain socket, you must modify a redis configuration file (typically located at /etc/redis/redis.conf or /etc/redis.conf) and restart the redis.service:

unixsocket /var/run/redis/redis-server.sock # specify the path to unix socket
unixsocketperm 700 # usually 700 or 770
EOF
)"
        exit 1
    else
        cecho "GREEN" "Redis configuration for unix domain socket is applied correctly."
    fi
fi


######### python package installation ########
if [[ " $@ " == *" -c "* ]]; then
    # controller
    REQUIREMENTS_TXT="requirements_controller.txt"
else
    # agent
    REQUIREMENTS_TXT="requirements_agent.txt"
fi

cecho "YELLOW" "Installing Python requirements from $REQUIREMENTS_TXT..."


if python3 -c "import pip" &>/dev/null; then
    cecho "GREEN" "python3 pip module is already installed."
else
    cecho "YELLOW" "pip module is not installed. Install python3-pip package..."
    if ! sudo dnf install -y python3-pip; then
        cecho "RED" "Error: Failed to install python3-pip package."
        exit 1
    fi
fi

REQUIREMENTS_FILE="$SCRIPT_DIR/$REQUIREMENTS_TXT"
if [[ -f "$REQUIREMENTS_FILE" ]]; then
    python3 -m pip install -r "$REQUIREMENTS_FILE" || {
        cecho "RED" "Error: Failed to install Python requirements."
        exit 1
    }
    cecho "GREEN" "Python requirements installed successfully."
else
    cecho "RED" "Error: $REQUIREMENTS_TXT not found at $REQUIREMENTS_FILE."
    exit 1
fi


###### RDMA CORE ######
cecho "YELLOW" "Checking RDMA Core and related packages installation..."

if [[ -f /etc/redhat-release ]]; then
    # RHEL-based system
    PACKAGES=("rdma-core" "rdma-core-devel")
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
    PACKAGES=("rdma-core" "libibverbs-dev")
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


######### INSTALL ########
# --- Determine resource limits based on -c flag ---
if [[ " $@ " == *" -c "* ]]; then
    CPU_QUOTA="1200%"
    MEM_MAX="16G"
else
    CPU_QUOTA="400%"
    MEM_MAX="2G"
fi

cecho "YELLOW" "Setting CPUQuota to $CPU_QUOTA and MemoryMax to $MEM_MAX in the service file..."

# --- Modify the service file using sed ---
sed -e "s/^CPUQuota=.*/CPUQuota=${CPU_QUOTA}/" \
    -e "s/^MemoryMax=.*/MemoryMax=${MEM_MAX}/" \
    "$SCRIPT_DIR/pingweave.service" > /tmp/pingweave.service

# --- Copy the modified service file ---
sudo cp /tmp/pingweave.service /etc/systemd/system/pingweave.service || {
    cecho "RED" "Error: Failed to copy modified pingweave.service to /etc/systemd/system/."
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

