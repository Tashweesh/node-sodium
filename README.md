# node-sodium


Port of the [lib sodium](https://github.com/jedisct1/libsodium) Encryption Library to Node.js.

This a work in progress but most of Lib Sodium as been ported already.
Missing are the `generichash` functions, and the alternative primitives, like `crypto_box_curve25519xsalsa20poly1305`, or `crypto_stream_aes128ctr`

There's a "low level" native module that gives you access directly to Lib Sodium, and a friendlier high level API that makes the use of the library a bit easier.

Check [`docs/ported-functions.md`](https://github.com/paixaop/node-sodium/tree/master/docs/ported-functions.md) for a list of all lib sodium functions included in node-sodium.

# Usage

Just a quick example that uses the same public/secret key pair to encrypt and then decrypt the message.

    var sodium = require('sodium');
    var box = new sodium.Box();     // random key pair, and nonce generated automatically

    var cipherText = box.encrypt("This is a secret message", "utf8");
    var plainText = box.decrypt(cipherText);


# Use the `KeyRing`

Because javascript uses garbage collecting, it could be considered as unsafe to store long term private keys into JS variables. Indeed, we have no control over the time that this kind of data is kept in memory before being effectively deleted (even though all references to such variable might have been deleted/lost a long time ago).

Hence we build `sodium.KeyRing` that will generate, hold the private key and do the cryptographic operations that require it. It also load/save keypairs into files on the disk. We can extract the public key from the `KeyRing`, but not the private key.

Check [`docs/keyring-api.md`](https://github.com/Mowje/node-sodium/tree/master/docs/keyring-api.md) for the list of methods of `KeyRing` and more details on how to use this class.

# Low Level API
A low level API is provided for advanced users. The functions available through the low level API have the exact same names as in lib sodium, and are available via the `sodium.api` object. Here is one example of how to use some of the low level API functions to encrypt/decrypt a message:

    var sodium = require('sodium').api;

    // Generate keys
    var sender = sodium.crypto_box_keypair();
    var receiver = sodium.crypto_box_keypair();

	// Generate random nonce
    var nonce = new Buffer(sodium.crypto_box_NONCEBYTES);
	sodium.randombytes_buf(nonce);

    // Encrypt
    var plainText = new Buffer('this is a message');
    var cipherMsg = sodium.crypto_box(plainText, nonce, receiver.publicKey, sender.secretKey);

    // Decrypt
    var plainBuffer = sodium.crypto_box_open(cipherMsg,nonce,sender.publicKey, receiver.secretKey);

    // We should get the same plainText!
    // We should get the same plainText!
    if( plainBuffer.toString() == plainText) {
        console.log("Message decrypted correctly");
    }

As you can see the high level API implementation is easier to use, but the low level API will fill just right for those with experience with the C version of lib sodium. It also allows you to bypass any bugs in the high level APIs.

You can find this code sample in `examples\low-level-api.js`.

# Documentation
Please read the work in progress documentation found under [`docs/`](https://github.com/paixaop/node-sodium/tree/master/docs).

You shoudld also review the unit tests as most of the high level API is "documented" there.
Don't forget to check out the examples as well.

# Libsodium Documentation

Libsodium is documented [here](https://download.libsodium.org/doc/). This module follows the same structure for original libsodium methods.

# Install

Tested on Mac and Linux. However be sure to have installed automake and libtool (libsodium dependencies) before installing this node module

    npm install git+ssh://git@github.com:Mowje/node-sodium.git

Or

    npm install git+https://github.com/Mowje/node-sodium.git

node-sodium depends on lib sodium, so if lib sodium does not compile on your platform chances are that process will fail.

# Manual Install
Clone this git repository, and change to the local directory where you ran git clone to,

    npm install

This will pull lib sodium from github and compile it by running the following commands

    cd libsodium
    ./autogen
    ./configure
    make

Followed by

    cd ..
    npm build .
    npm install

If you get an `autogen.sh` error similar to this

    ./autogen.sh: line 13: libtoolize: command not found

You'll need to install libtool and automake in your platform. For Mac OSX you can use [Homebrew](http://brew.sh)

    brew install libtool automake

Then repeat the steps from `./autogen.sh`

## `cannot open shared object file` error

This error might happen on your first call to node-sodium (whether it be from test scripts or in-app). To fix it, setting environment variables as written below should do the trick (as suggested [here](https://github.com/paixaop/node-sodium/issues/4))

    export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:$(pwd)/node_modules/sodium/libsodium/src/libsodium/.libs/
    export LD_RUN_PATH=$LD_RUN_PATH:$(pwd)/node_modules/sodium/libsodium/src/libsodium/.libs/

This error has reportedly happened on different Linux distros after a successful compilation

# Building for node-webkit

This feature hasn't been maintained in a long while. Note that it is possible to do it, but since the "wonderful" JS chain of events (Node & IO.js fork & merge , node-webkit renamed to nw.js, and the electron project launch) I haven't re-tested it.

~~It is possible to build this module for [node-webkit](https://github.com/rogerwang/node-webkit). As of now, the newest possible target is `0.8.6`, because newer versions of node-webkit use a currently unstable version of nodejs on which the current code cannot build yet.~~

~~On OSX, you can build an example app by calling `make build-test-nw-osx`; you'll find the resulting app in `build/nw/`~~

# Code Samples
Please check the fully documented code samples in `test/test_sodium.js`.

# Installing Mocha Test Suite

To run the unit tests you need Mocha. If you'd like to run coverage reports you need mocha-istanbul. You can install both globally by doing

    npm install -g mocha mocha-istanbul

You may need to run it with `sudo` is only root user has access to Node.js global directories

    sudo npm install -g mocha mocha-istanbul

# Unit Tests
You need to have mocha test suite installed globally then you can run the node-sodium unit tests by

    make test

# Coverage Reports
You need to have mocha test suite installed globally then you can run the node-sodium unit tests by

    make test-cov
