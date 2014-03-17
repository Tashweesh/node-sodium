//#define BUILDING_NODE_EXTENSION

#include <iostream>

#include <string>
#include <map>
#include <exception>
#include <stdexcept>
#include <fstream>
#include <sstream>
#include <utility>
#include <iomanip>
#include <algorithm>

#include <node.h>
#include <node_buffer.h>
#include "keyring.h"

//Including libsodium export headers
#include "sodium.h"

using namespace v8;
using namespace node;
using namespace std;

#define PREPARE_FUNC_VARS() \
	HandleScope scope; \
	KeyRing* instance = ObjectWrap::Unwrap<KeyRing>(args.This()); \
	Local<Object> globalObj = Context::GetCurrent()->Global();

//Defining "invlid number of parameters" macro
#define MANDATORY_ARGS(n, message) \
	if (args.Length() < n){ \
		ThrowException(Exception::TypeError(String::New(message))); \
		return scope.Close(Undefined()); \
	}

#define CHECK_KEYPAIR(type) \
	if (instance->keyPair == 0){ \
		ThrowException(Exception::TypeError(String::New("No key pair has been loaded into the key ring"))); \
		return scope.Close(Undefined()); \
	} \
	if (instance->keyPair->at("keyType") != type){ \
		ThrowException(Exception::TypeError(String::New("Invalid key type"))); \
		return scope.Close(Undefined()); \
	}

#define BUILD_BUFFER(data) \
	int resultBufferLength = strlen(data); \
	Buffer* slowBuffer = Buffer::New(resultBufferLength); \
	memcpy(Buffer::Data(slowBuffer), data, resultBufferLength); \
	Local<Function> bufferConstructor = Local<Function>::Cast(globalObj->Get(String::New("Buffer"))); \
	Handle<Value> constructorArgs[3] = { slowBuffer->handle_, Integer::New(resultBufferLength), Integer::New(0) }; \
	Local<Object> resultBuffer = bufferConstructor->NewInstance(3, constructorArgs);

Persistent<Function> KeyRing::constructor;

KeyRing::KeyRing(string filename) : keyPair(0), filename_(filename){
	if (filename != ""){
		if (!doesFileExist(filename)){
			//Throw a V8 exception??
			return;
		}
		keyPair = loadKeyPair(filename);
		filename_ = filename;
	}
}

KeyRing::~KeyRing(){
	if (keyPair != 0){
		delete keyPair;
		keyPair = 0;
	}
}

void KeyRing::Init(Handle<Object> exports){
	//Prepare constructor template
	Local<FunctionTemplate> tpl = FunctionTemplate::New(New);
	tpl->SetClassName(String::NewSymbol("KeyRing"));
	tpl->InstanceTemplate()->SetInternalFieldCount(2);
	//Prototype
	tpl->PrototypeTemplate()->Set(String::NewSymbol("encrypt"), FunctionTemplate::New(Encrypt)->GetFunction());
	tpl->PrototypeTemplate()->Set(String::NewSymbol("decrypt"), FunctionTemplate::New(Decrypt)->GetFunction());
	tpl->PrototypeTemplate()->Set(String::NewSymbol("sign"), FunctionTemplate::New(Sign)->GetFunction());
	tpl->PrototypeTemplate()->Set(String::NewSymbol("agree"), FunctionTemplate::New(Agree)->GetFunction());
	tpl->PrototypeTemplate()->Set(String::NewSymbol("publicKeyInfo"), FunctionTemplate::New(PublicKeyInfo)->GetFunction());
	tpl->PrototypeTemplate()->Set(String::NewSymbol("createKeyPair"), FunctionTemplate::New(CreateKeyPair)->GetFunction());
	tpl->PrototypeTemplate()->Set(String::NewSymbol("load"), FunctionTemplate::New(Load)->GetFunction());
	tpl->PrototypeTemplate()->Set(String::NewSymbol("save"), FunctionTemplate::New(Save)->GetFunction());
	tpl->PrototypeTemplate()->Set(String::NewSymbol("clear"), FunctionTemplate::New(Clear)->GetFunction());

	constructor = Persistent<Function>::New(tpl->GetFunction());
	exports->Set(String::NewSymbol("KeyRing"), constructor);
}

/*
* JS -> C++ data constructor bridge
*/
Handle<Value> KeyRing::New(const Arguments& args){
	HandleScope scope;
	if (args.IsConstructCall()){
		//Invoked as a constructor
		string filename;
		if (args[0]->IsUndefined()){
			filename = "";
		} else {
			String::Utf8Value filenameVal(args[0]->ToString());
			filename = string(*filenameVal);
		}
		KeyRing* newInstance = new KeyRing(filename);
		newInstance->Wrap(args.This());
		return args.This();
	} else {
		//Invoked as a plain function; turn it into construct call
		if (args.Length() > 1){
			ThrowException(Exception::TypeError(String::New("Invalid number of arguments on KeyRing constructor call")));
			return scope.Close(Undefined());
		}
		if (args.Length() == 1){
			Local<Value> argv[1] = { args[0] };
			return scope.Close(constructor->NewInstance(1, argv));
		} else {
			return scope.Close(constructor->NewInstance());
		}
	}
}

/**
* Make a Curve25519 key exchange for a given public key, then encrypt the message (crypto_box)
* Parameters Buffer message, Buffer publicKey, Buffer nonce, callback (optional)
* Returns Buffer
*/
Handle<Value> KeyRing::Encrypt(const Arguments& args){
	PREPARE_FUNC_VARS();
	MANDATORY_ARGS(3, "Mandatory args : message, counterpartPubKey, nonce\nOptional args: callback");
	CHECK_KEYPAIR("curve25519");

	Local<Object> messageVal = args[0]->ToObject();
	Local<Object> publicKeyVal = args[1]->ToObject();
	Local<Object> nonceVal = args[2]->ToObject();

	const unsigned char* message = (unsigned char*) Buffer::Data(messageVal);
	const size_t messageLength = Buffer::Length(messageVal);

	const unsigned char* publicKey = (unsigned char*) Buffer::Data(publicKeyVal);
	const size_t publicKeyLength = Buffer::Length(publicKeyVal);
	if (publicKeyLength != crypto_box_PUBLICKEYBYTES){
		stringstream errMsg;
		errMsg << "Public key must be " << crypto_box_PUBLICKEYBYTES << " bytes long";
		ThrowException(Exception::TypeError(String::New(errMsg.str().c_str())));
		return scope.Close(Undefined());
	}
	const unsigned char* nonce = (unsigned char*) Buffer::Data(nonceVal);
	const size_t nonceLength = Buffer::Length(nonceVal);
	if (nonceLength != crypto_box_NONCEBYTES){
		stringstream errMsg;
		errMsg << "The nonce must be " << crypto_box_NONCEBYTES << " bytes long";
		ThrowException(Exception::TypeError(String::New(errMsg.str().c_str())));
		return scope.Close(Undefined());
	}

	unsigned char* paddedMessage = new unsigned char[messageLength + crypto_box_ZEROBYTES];
	for (unsigned int i = 0; i < crypto_box_ZEROBYTES; i++){
		paddedMessage[i] = 0;
	}
	memcpy((void*) (paddedMessage + crypto_box_ZEROBYTES), (void*) message, messageLength);

	Buffer* cipherBuf = Buffer::New(messageLength + crypto_box_ZEROBYTES);
	unsigned char* cipher = (unsigned char*)Buffer::Data(cipherBuf);
	string privateKey = instance->keyPair->at("privateKey");
	privateKey = hexToStr(privateKey);

	int boxResult = crypto_box(cipher, paddedMessage, messageLength + crypto_box_ZEROBYTES, nonce, publicKey, (unsigned char*) privateKey.c_str());
	if (boxResult != 0){
		stringstream errMsg;
		errMsg << "Error while encrypting message. Error code : " << boxResult;
		ThrowException(Exception::TypeError(String::New(errMsg.str().c_str())));
		return scope.Close(Undefined());
	}

	//BUILD_BUFFER(string((char*) cipher, message.length()).c_str());
	if (args.Length() == 3){
		return scope.Close(cipherBuf->handle_);
	} else {
		BUILD_BUFFER(string((char*)cipher, messageLength + crypto_box_ZEROBYTES).c_str());
		Local<Function> callback = Local<Function>::Cast(args[3]);
		const int argc = 1;
		Local<Value> argv[argc] = { resultBuffer };
		callback->Call(globalObj, argc, argv);
		return scope.Close(Undefined());
	}
}

/*
* Decrypt a message, using crypto_box_open
* Args : Buffer cipher, Buffer publicKey, Buffer nonce, Function callback (optional)
*/
Handle<Value> KeyRing::Decrypt(const Arguments& args){
	PREPARE_FUNC_VARS();
	MANDATORY_ARGS(3, "Mandatory args : cipher, counterpartPubKey, nonce\nOptional args: callback");
	CHECK_KEYPAIR("curve25519");

	Local<Object> cipherVal = args[0]->ToObject();
	Local<Object> publicKeyVal = args[1]->ToObject();
	Local<Object> nonceVal = args[2]->ToObject();

	const unsigned char* cipher = (unsigned char*) Buffer::Data(cipherVal);
	const size_t cipherLength = Buffer::Length(cipherVal);

	//Checking that the first crypto_box_BOXZEROBYTES are zeros
	unsigned int i = 0;
	for (i = 0; i < crypto_box_BOXZEROBYTES; i++){
		if (cipher[i]) break;
	}
	if (i < crypto_box_BOXZEROBYTES){
		stringstream errMsg;
		errMsg << "The first " << crypto_box_BOXZEROBYTES << " bytes of the cipher argument must be zeros";
		ThrowException(Exception::TypeError(String::New(errMsg.str().c_str())));
		return scope.Close(Undefined());
	}

	const unsigned char* publicKey = (unsigned char*) Buffer::Data(publicKeyVal);

	const unsigned char* nonce = (unsigned char*) Buffer::Data(nonceVal);

	unsigned char* message = new unsigned char[cipherLength];
	string privateKey = instance->keyPair->at("privateKey");
	privateKey = hexToStr(privateKey);

	int boxResult = crypto_box_open(message, cipher, cipherLength, nonce, publicKey, (unsigned char*) privateKey.c_str());
	if (boxResult != 0){
		stringstream errMsg;
		errMsg << "Error while decrypting message. Error code : " << boxResult;
		ThrowException(Exception::TypeError(String::New(errMsg.str().c_str())));
		return scope.Close(Undefined());
	}

	unsigned char* plaintext = new unsigned char[cipherLength - crypto_box_ZEROBYTES];
	memcpy(plaintext, (void*) (message + crypto_box_ZEROBYTES), cipherLength - crypto_box_ZEROBYTES);


	BUILD_BUFFER(string((char*)plaintext, cipherLength - crypto_box_ZEROBYTES).c_str());
	if (args.Length() == 3){
		return scope.Close(resultBuffer);
	} else {
		Local<Function> callback = Local<Function>::Cast(args[3]);
		const int argc = 1;
		Local<Value> argv[argc] = { resultBuffer };
		callback->Call(globalObj, argc, argv);
		return scope.Close(Undefined());
	}
}

/*
* Sign a given message, using crypto_sign
* Args: Buffer message, Function callback (optional)
*/
Handle<Value> KeyRing::Sign(const Arguments& args){
	PREPARE_FUNC_VARS();
	MANDATORY_ARGS(1, "Mandatory args : message\nOptional args: callback");
	CHECK_KEYPAIR("ed25519");

	Local<Value> messageVal = args[0]->ToObject();

	const unsigned char* message = (unsigned char*) Buffer::Data(messageVal);
	const size_t messageLength = Buffer::Length(messageVal);

	Buffer* signatureBuf = Buffer::New(messageLength + crypto_sign_BYTES);
	unsigned char* signature = (unsigned char*) Buffer::Data(signatureBuf);
	unsigned long long signatureSize;

	string privateKey = instance->keyPair->at("privateKey");
	privateKey = hexToStr(privateKey);

	int signResult = crypto_sign(signature, &signatureSize, message, messageLength, (unsigned char*) privateKey.c_str());
	if (signResult != 0){
		stringstream errMsg;
		errMsg << "Error while signing the message. Code : " << signResult << endl;
		ThrowException(Exception::TypeError(String::New(errMsg.str().c_str())));
		return scope.Close(Undefined());
	}

	//BUILD_BUFFER(string((char*) signature, messageLength + crypto_sign_BYTES).c_str());

	if (args.Length() == 1){
		return scope.Close(signatureBuf->handle_); 
	} else {
		BUILD_BUFFER(string((char*) signature, messageLength + crypto_sign_BYTES).c_str());
		Local<Function> callback = Local<Function>::Cast(args[1]);
		const int argc = 1;
		Local<Value> argv[argc] = { resultBuffer };
		callback->Call(globalObj, argc, argv);
		return scope.Close(Undefined());
	}
}

/*
* Do a Curve25519 key-exchange
* Args : Buffer counterpartPubKey, Function callback (optional)
*/
Handle<Value> KeyRing::Agree(const Arguments& args){
	PREPARE_FUNC_VARS();
	MANDATORY_ARGS(1, "Mandatory args : counterpartPubKey\nOptional: callback");
	CHECK_KEYPAIR("curve25519");

	Local<Object> publicKeyVal = args[0]->ToObject();
	const unsigned char* counterpartPubKey = (unsigned char*) Buffer::Data(publicKeyVal);

	string privateKey = instance->keyPair->at("privateKey");
	privateKey = hexToStr(privateKey);

	Buffer* sharedSecretBuf = Buffer::New(crypto_scalarmult_BYTES);
	unsigned char* sharedSecret = (unsigned char*) Buffer::Data(sharedSecretBuf);
	crypto_scalarmult(sharedSecret, (unsigned char*) privateKey.c_str(), counterpartPubKey);

	if (args.Length() == 1){
		return scope.Close(sharedSecretBuf->handle_);
	} else {
		BUILD_BUFFER(string((char*) sharedSecret, crypto_scalarmult_BYTES).c_str());
		Local<Function> callback = Local<Function>::Cast(args[1]);
		const int argc = 1;
		Local<Value> argv[argc] = { resultBuffer };
		callback->Call(globalObj, argc, argv);
		return scope.Close(Undefined());
	}
}

//Function callback (optional)
Handle<Value> KeyRing::PublicKeyInfo(const Arguments& args){
	PREPARE_FUNC_VARS();
	//Checking that a keypair is loaded in memory
	if (instance->keyPair == 0){
		ThrowException(Exception::TypeError(String::New("No key has been loaded into memory")));
		return scope.Close(Undefined());
	}
	//Sync/async fork
	if (args.Length() == 0){
		return scope.Close(instance->PPublicKeyInfo());
	} else {
		Local<Function> callback = Local<Function>::Cast(args[0]);
		const unsigned argc = 1;
		Local<Value> argv[argc] = { Local<Value>::New(instance->PPublicKeyInfo()) };
		callback->Call(globalObj, argc, argv);
		return scope.Close(Undefined());
	}
}

Local<Object> KeyRing::PPublicKeyInfo(){
	Local<Object> pubKeyObj = Object::New();
	if (keyPair == 0){
		throw new runtime_error("No loaded key pair");
	}
	string keyType = keyPair->at("keyType");
	string publicKey = keyPair->at("publicKey");
	pubKeyObj->Set(String::NewSymbol("keyType"), String::New(keyType.c_str()));
	pubKeyObj->Set(String::NewSymbol("publicKey"), String::New(publicKey.c_str()));
	return pubKeyObj;
}

/*
* Generates a keypair. Save it to filename if given
* String keyType, String filename [optional], Function callback [optional]
*/
Handle<Value> KeyRing::CreateKeyPair(const Arguments& args){
	PREPARE_FUNC_VARS();
	MANDATORY_ARGS(1, "Please give the type of the key you want to generate");
	String::Utf8Value keyTypeVal(args[0]->ToString());
	string keyType(*keyTypeVal);
	if (!(keyType == "ed25519" || keyType == "curve25519")) {
		ThrowException(Exception::TypeError(String::New("Invalid key type")));
		return scope.Close(Undefined());
	}
	//Preparing new keypair map
	map<string, string>* newKeyPair = new map<string, string>();
	//Delete the keypair loaded in memory, if any
	if (instance->keyPair != 0){
		delete instance->keyPair;
		instance->keyPair = 0;
	}
	instance->keyPair = newKeyPair;
	if (keyType == "ed25519"){
		unsigned char* privateKey = new unsigned char[crypto_sign_SECRETKEYBYTES];
		unsigned char* publicKey = new unsigned char[crypto_sign_PUBLICKEYBYTES];
		crypto_sign_keypair(publicKey, privateKey);

		newKeyPair->insert(make_pair("keyType", "ed25519"));
		newKeyPair->insert(make_pair("privateKey", strToHex(string((char *)privateKey, crypto_sign_SECRETKEYBYTES))));
		newKeyPair->insert(make_pair("publicKey", strToHex(string((char *)publicKey, crypto_sign_PUBLICKEYBYTES))));
		delete privateKey;
		delete publicKey;
	} else if (keyType == "curve25519"){
		unsigned char* privateKey = new unsigned char[crypto_box_SECRETKEYBYTES];
		unsigned char* publicKey = new unsigned char[crypto_box_PUBLICKEYBYTES];
		crypto_box_keypair(publicKey, privateKey);

		newKeyPair->insert(make_pair("keyType", "curve25519"));
		newKeyPair->insert(make_pair("privateKey", strToHex(string((char *)privateKey, crypto_box_SECRETKEYBYTES))));
		newKeyPair->insert(make_pair("publicKey", strToHex(string((char *)publicKey, crypto_box_PUBLICKEYBYTES))));
		delete privateKey;
		delete publicKey;
	}

	if (args.Length() >= 2 && !args[1]->IsUndefined()){ //Save keypair to file
		String::Utf8Value filenameVal(args[1]->ToString());
		string filename(*filenameVal);
		saveKeyPair(filename, instance->keyPair);
		instance->filename_ = filename;
	}
	if (args.Length() == 3){ //Callback
		Local<Function> callback = Local<Function>::Cast(args[2]);
		const unsigned argc = 1;
		Local<Value> argv[argc] = { Local<Value>::New(instance->PPublicKeyInfo()) };
		callback->Call(globalObj, argc, argv);
		return scope.Close(Undefined());
	} else {
		return scope.Close(instance->PPublicKeyInfo());
	}
}

// String filename, Function callback (optional)
Handle<Value> KeyRing::Load(const Arguments& args){
	PREPARE_FUNC_VARS();
	MANDATORY_ARGS(1, "Mandatory args : String filename\nOptional args : Function callback");

	String::Utf8Value filenameVal(args[0]->ToString());
	string filename(*filenameVal);

	map<string, string>* newKeyPair;
	try {
		newKeyPair = loadKeyPair(filename);
	} catch (runtime_error* e){
		string errMsg = e->what();
		ThrowException(Exception::TypeError(String::New(errMsg.c_str())));
		return scope.Close(Undefined());
	} catch (void* e){
		ThrowException(Exception::TypeError(String::New("Error while loading the key file")));
		return scope.Close(Undefined());
	}

	if (instance->keyPair != 0){
		delete instance->keyPair;
		instance->keyPair = 0;
	}
	instance->keyPair = newKeyPair;
	instance->filename_ = filename;

	if (args.Length() == 1){
		return scope.Close( instance->PPublicKeyInfo() );
	} else {
		Local<Function> callback = Local<Function>::Cast(args[1]);
		const int argc = 1;
		Local<Value> argv[argc] = { Local<Value>::New(instance->PPublicKeyInfo()) };
		callback->Call(globalObj, argc, argv);
		return scope.Close(Undefined());
	}
}

// String filename, Function callback (optional)
Handle<Value> KeyRing::Save(const Arguments& args){
	PREPARE_FUNC_VARS();
	MANDATORY_ARGS(1, "Mandatory args : String filename\nOptional args : Function callback");

	if (instance->keyPair == 0){ //Checking that a key is indeed defined. If not, throw an exception
		ThrowException(Exception::TypeError(String::New("No key has been loaded into the keyring")));
		return scope.Close(Undefined());
	}

	String::Utf8Value filenameVal(args[0]);
	string filename(*filenameVal);

	try {
		saveKeyPair(filename, instance->keyPair);
	} catch (runtime_error* e){
		string errMsg = e->what();
		ThrowException(Exception::TypeError(String::New(errMsg.c_str())));
		return scope.Close(Undefined());
	} catch (void* e){
		ThrowException(Exception::TypeError(String::New("Error while saving the key file")));
		return scope.Close(Undefined());
	}

	if (args.Length() == 1){
		return scope.Close(Undefined());
	} else {
		Local<Function> callback = Local<Function>::Cast(args[1]);
		const int argc = 0;
		Local<Value> argv[argc];
		callback->Call(globalObj, argc, argv);
		return scope.Close(Undefined());
	}
}

Handle<Value> KeyRing::Clear(const Arguments& args){
	HandleScope scope;
	KeyRing* instance = ObjectWrap::Unwrap<KeyRing>(args.This());
	if (instance->keyPair != 0){
		delete instance->keyPair;
		instance->keyPair = 0;
	}
	return scope.Close(Undefined());
}

/*map<string, string>* KeyRing::edwardsToMontgomery(map<string, string>* edwards){

}*/

string KeyRing::strToHex(string const& s){
	static const char* const charset = "0123456789abcdef";
	size_t length = s.length();

	string output;
	output.reserve(2 * length);
	for (size_t i = 0; i < length; i++){
	   const unsigned char c = s[i];
	   output.push_back(charset[c >> 4]);
	   output.push_back(charset[c & 15]);
	}
	return output;
}

string KeyRing::hexToStr(string const& s){
	static const char* const charset = "0123456789abcdef";
    size_t length = s.length();
    if (length & 1) throw invalid_argument("Odd length");
    
    string output;
    output.reserve(length / 2);
    for (size_t i = 0; i < length; i+= 2){
        char a = s[i];
        const char* p = lower_bound(charset, charset + 16, a);
        if (*p != a) throw invalid_argument("Invalid hex char");
        
        char b = s[i + 1];
        const char* q = lower_bound(charset, charset + 16, b);
        if (*q != b) throw invalid_argument("Invalid hex char");
        
        output.push_back(((p - charset) << 4) | (q - charset));
    }
    return output;
}

bool KeyRing::doesFileExist(string const& filename){
	fstream file(filename.c_str(), ios::in);
	bool isGood = file.good();
	file.close();
	return isGood;
}

void KeyRing::saveKeyPair(string const& filename, map<string, string>* keyPair){
	fstream fileWriter(filename.c_str(), ios::out | ios::trunc);
	string params[] = {"keyType", "privateKey", "publicKey"};
	for (int i = 0; i < 3; i++){
		if (!(keyPair->count(params[i]) > 0)) throw new runtime_error("Missing parameter when saving file : " + params[i]);
	}
	string* keyType = &(keyPair->at("keyType"));
	string* privateKey = &(keyPair->at("privateKey"));
	string* publicKey = &(keyPair->at("publicKey"));
	if (*keyType == "curve25519"){
		//Writing key type
		fileWriter << (char) 0x05;
		//Writing public key length
		fileWriter << (char) (crypto_box_PUBLICKEYBYTES >> 8);
		fileWriter << (char) (crypto_box_PUBLICKEYBYTES);
		//Writing public key
		fileWriter << hexToStr(*publicKey);
		//Writing private key length
		fileWriter << (char) (crypto_box_SECRETKEYBYTES >> 8);
		fileWriter << (char) crypto_box_SECRETKEYBYTES;
		//Writing the private key
		fileWriter << hexToStr(*privateKey);
	} else if (*keyType == "ed25519"){
		//Writing key type
		fileWriter << (char) 0x06;
		//Writing public key length
		fileWriter << (char) (crypto_sign_PUBLICKEYBYTES >> 8);
		fileWriter << (char) crypto_sign_PUBLICKEYBYTES;
		//Writing the public key
		fileWriter << hexToStr(*publicKey);
		//Writing private key length
		fileWriter << (char) (crypto_sign_SECRETKEYBYTES >> 8);
		fileWriter << (char) crypto_sign_SECRETKEYBYTES;
		//Writing the private key
		fileWriter << hexToStr(*privateKey);
	} else throw new runtime_error("Unknown key type: " + *keyType);
	fileWriter.close();
}

map<string, string>* KeyRing::loadKeyPair(string const& filename){
	fstream fileReader(filename.c_str(), ios::in);
	string keyStr;
	getline(fileReader, keyStr);
	fileReader.close();
	stringstream keyStream(keyStr);
	stringbuf* buffer = keyStream.rdbuf();
	//Declaring the keyPair map
	map<string, string>* keyPair;
	//Reading the keytype
	char keyType = buffer->sbumpc();
	if (!(keyType == 0x05 || keyType == 0x06)){ //Checking that the key type is valid
		stringstream errMsg;
		errMsg << "Invalid key type: " << (int) keyType;
		throw new runtime_error(errMsg.str());
	}
	keyPair = new map<string, string>();
	unsigned int publicKeyLength, privateKeyLength;
	string publicKey = "", privateKey = "";
	if (keyType == 0x05){ //Curve25519
		//Getting public key length
		publicKeyLength = ((unsigned int) buffer->sbumpc()) << 8;
		publicKeyLength += (unsigned int) buffer->sbumpc();
		if (publicKeyLength != crypto_box_PUBLICKEYBYTES){ //Checking key length
			stringstream errMsg;
			errMsg << "Invalid public key length : " << publicKeyLength;
			throw new runtime_error(errMsg.str());
		}
		//Getting public key
		for (unsigned int i = 0; i < publicKeyLength; i++){
			publicKey += (char) buffer->sbumpc();
		}
		publicKey = strToHex(publicKey);
		//Getting private key length
		privateKeyLength = ((unsigned int) buffer->sbumpc()) << 8;
		privateKeyLength += (unsigned int) buffer->sbumpc();
		if (privateKeyLength != crypto_box_SECRETKEYBYTES){ //Checking key length
			stringstream errMsg;
			errMsg << "Invalid private key length : " << privateKeyLength;
			throw new runtime_error(errMsg.str()); 
		}
		//Getting private key
		for (unsigned int i = 0; i < privateKeyLength; i++){
			privateKey += (char) buffer->sbumpc();
		}
		privateKey = strToHex(privateKey);
		//Building keypair map
		keyPair->insert(make_pair("keyType", "curve25519"));
		keyPair->insert(make_pair("publicKey", publicKey));
		keyPair->insert(make_pair("privateKey", privateKey));
	} else if (keyType == 0x06){ //Ed25519
		//Getting public key length
		publicKeyLength = ((unsigned int) buffer->sbumpc()) << 8;
		publicKeyLength += (unsigned int) buffer->sbumpc();
		if (publicKeyLength != crypto_sign_PUBLICKEYBYTES){ //Checking key length
			stringstream errMsg;
			errMsg << "Invalid public key length : " << publicKeyLength;
			throw new runtime_error(errMsg.str());
		}
		//Getting public key
		for (unsigned int i = 0; i < publicKeyLength; i++){
			publicKey += (char) buffer->sbumpc();
		}
		publicKey = strToHex(publicKey);
		//Getting private key length
		privateKeyLength = ((unsigned int) buffer->sbumpc()) << 8;
		privateKeyLength += (unsigned int) buffer->sbumpc();
		if (privateKeyLength != crypto_sign_SECRETKEYBYTES){ //Cheking key length
			stringstream errMsg;
			errMsg << "Invalid private key length : " << privateKeyLength;
			throw new runtime_error(errMsg.str());
		}
		//Getting private key
		for (unsigned int i = 0; i < privateKeyLength; i++){
			privateKey += (char) buffer->sbumpc();
		}
		privateKey = strToHex(privateKey);
		//Building keypair map
		keyPair->insert(make_pair("keyType", "ed25519"));
		keyPair->insert(make_pair("publicKey", publicKey));
		keyPair->insert(make_pair("privateKey", privateKey));
	}
	return keyPair;
}