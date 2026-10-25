#!/bin/sh

# Check if the first argument is "utils"
if [ "$1" = "utils" ]; then
    mkdir -p deploy/config/overlay/etc
    utils/openmiko-gen.sh deploy/config/overlay/etc
    utils/wpa-gen.sh deploy/config/overlay/etc
    echo "Done."
    exit 0
fi

# Check if the first argument is "passwd"
if [ "$1" = "passwd" ]; then
    mkdir -p deploy/config/overlay/etc
    echo "root:x:0:0:root:/root:/bin/sh" > deploy/config/overlay/etc/passwd

    # Prompt for the password
    read -s -p "Enter password for root: " PASSWORD
    echo

    # Generate a password hash
    PASSWORD_HASH=$(openssl passwd -1 "$PASSWORD")

    # Add the hashed password to the passwd file
    sed -i '' "s|root:x:0:0:root:/root:/bin/sh|root:$PASSWORD_HASH:0:0:root:/root:/bin/sh|" deploy/config/overlay/etc/passwd

    # Copy the current user's public SSH key to the target system
    mkdir -p deploy/config/overlay/root/.ssh
    if [ -f ~/.ssh/id_rsa.pub ]; then
        cp ~/.ssh/id_rsa.pub deploy/config/overlay/root/.ssh/authorized_keys
        chmod 600 deploy/config/overlay/root/.ssh/authorized_keys
    elif [ -f ~/.ssh/id_ecdsa.pub ]; then
        cp ~/.ssh/id_ecdsa.pub deploy/config/overlay/root/.ssh/authorized_keys
        chmod 600 deploy/config/overlay/root/.ssh/authorized_keys
    elif [ -f ~/.ssh/id_ed25519.pub ]; then
        cp ~/.ssh/id_ed25519.pub deploy/config/overlay/root/.ssh/authorized_keys
        chmod 600 deploy/config/overlay/root/.ssh/authorized_keys
    else
        echo "No SSH public key found in ~/.ssh"
        exit 1
    fi
    echo "Done."
    exit 0
fi

# Otherwise, print usage
echo "Usage: $0 [utils|passwd]"