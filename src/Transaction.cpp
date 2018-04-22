/*
 * FoundationDB Node.js API
 * Copyright (c) 2012 FoundationDB, LLC
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <node.h>
#include <iostream>
#include <string>
#include <cstring>
#include <vector>
#include <node_buffer.h>
#include <node_version.h>

#include "options.h"
#include "Transaction.h"
#include "NodeCallback.h"
#include "FdbError.h"

#include "future.h"

using namespace v8;
using namespace std;
using namespace node;



// Transaction Implementation
Transaction::Transaction() { };

Transaction::~Transaction() {
  fdb_transaction_destroy(tr);
};

Nan::Persistent<Function> Transaction::constructor;



struct StringParams {
  bool owned;
  uint8_t *str;
  int len;

  /*
   *  String arguments always have to be buffers to
   *  preserve bytes. Otherwise, stuff gets converted
   *  to UTF-8.
   */
  StringParams(Local<Value> keyVal) {
    if (keyVal->IsString()) {
      owned = true;

      auto s = Local<String>::Cast(keyVal);
      len = s->Utf8Length();
      str = new uint8_t[len];
      s->WriteUtf8((char *)str, len);
    } else {
      owned = false;

      auto obj = keyVal->ToObject();
      str = (uint8_t*)(Buffer::Data(obj));
      len = (int)Buffer::Length(obj);
    }
  }
  ~StringParams() {
    if (owned) delete[] str;
  }
};




// **** Transaction

FDBTransaction* Transaction::GetTransactionFromArgs(const Nan::FunctionCallbackInfo<Value>& info) {
  return node::ObjectWrap::Unwrap<Transaction>(info.Holder())->tr;
}


static Local<Value> ignoreResult(FDBFuture* future, fdb_error_t* errOut) {
  *errOut = fdb_future_get_error(future);
  return Nan::Undefined();
}

static Local<Value> getValue(FDBFuture* future, fdb_error_t* errOut) {
  Isolate *isolate = Isolate::GetCurrent();

  const char *value;
  int valueLength;
  int valuePresent;

  *errOut = fdb_future_get_value(future, &valuePresent, (const uint8_t**)&value, &valueLength);
  if (*errOut) return Undefined(isolate);

  return valuePresent
    ? Local<Value>::New(isolate, Nan::CopyBuffer(value, valueLength).ToLocalChecked())
    : Local<Value>(Null(isolate));
}

static Local<Value> getKey(FDBFuture* future, fdb_error_t* errOut) {
  Isolate *isolate = Isolate::GetCurrent();

  const char *key;
  int keyLength;
  *errOut = fdb_future_get_key(future, (const uint8_t**)&key, &keyLength);

  if (*errOut) return Undefined(isolate);
  else return Local<Value>::New(isolate, Nan::CopyBuffer(key, keyLength).ToLocalChecked());
}

static Local<Value> getKeyValueList(FDBFuture* future, fdb_error_t* errOut) {
  Isolate *isolate = Isolate::GetCurrent();

  const FDBKeyValue *kv;
  int len;
  fdb_bool_t more;

  *errOut = fdb_future_get_keyvalue_array(future, &kv, &len, &more);
  if (*errOut) return Undefined(isolate);

  /*
   * Constructing a JavaScript object with:
   * { values: [{key, value}, {key, value}, ...], more }
   */

  Local<Object> returnObj = Local<Object>::New(isolate, Object::New(isolate));
  Local<Array> jsValueArray = Array::New(isolate, len);

  Local<String> keySymbol = String::NewFromUtf8(isolate, "key", String::kInternalizedString);
  Local<String> valueSymbol = String::NewFromUtf8(isolate, "value", String::kInternalizedString);

  for(int i = 0; i < len; i++) {
    Local<Object> jsKeyValue = Object::New(isolate);

    Local<Value> jsKeyBuffer = Nan::CopyBuffer((const char*)kv[i].key, kv[i].key_length).ToLocalChecked();
    Local<Value> jsValueBuffer = Nan::CopyBuffer((const char*)kv[i].value, kv[i].value_length).ToLocalChecked();

    jsKeyValue->Set(keySymbol, jsKeyBuffer);
    jsKeyValue->Set(valueSymbol, jsValueBuffer);
    jsValueArray->Set(Number::New(isolate, i), jsKeyValue);
  }

  returnObj->Set(String::NewFromUtf8(isolate, "values", String::kInternalizedString), jsValueArray);
  returnObj->Set(String::NewFromUtf8(isolate, "more", String::kInternalizedString), Boolean::New(isolate, !!more));

  return returnObj;
}

static Local<Value> getStringArray(FDBFuture* future, fdb_error_t* errOut) {
  Isolate *isolate = Isolate::GetCurrent();

  const char **strings;
  int stringCount;

  *errOut = fdb_future_get_string_array(future, &strings, &stringCount);
  if (*errOut) return Undefined(isolate);

  Local<Array> jsArray = Local<Array>::New(isolate, Array::New(isolate, stringCount));
  for(int i = 0; i < stringCount; i++) {
    jsArray->Set(Number::New(isolate, i), Nan::New(strings[i], (int)strlen(strings[i])).ToLocalChecked());
  }

  return jsArray;
}

static Local<Value> getVersion(FDBFuture* future, fdb_error_t* errOut) {
  Isolate *isolate = Isolate::GetCurrent();

  int64_t version;
  *errOut = fdb_future_get_version(future, &version);

  //SOMEDAY: This limits the version to 53-bits. Is it worth writing this out
  //into a buffer instead?
  if (*errOut) return Undefined(isolate);
  else return Local<Value>::New(isolate, Number::New(isolate, (double)version));
}



// setOption(code, value).
void Transaction::SetOption(const Nan::FunctionCallbackInfo<v8::Value>& args) {
  // database.setOptionStr(opt_id, "value")
  FDBTransaction *tr = GetTransactionFromArgs(args);
  set_option_wrapped(tr, OptTransaction, args);
}


// commit()
void Transaction::Commit(const Nan::FunctionCallbackInfo<Value>& info) {
  FDBFuture *f = fdb_transaction_commit(GetTransactionFromArgs(info));
  info.GetReturnValue().Set(futureToJS(f, info[0], ignoreResult));
}

// Reset the transaction so it can be reused.
void Transaction::Reset(const Nan::FunctionCallbackInfo<Value>& info) {
  fdb_transaction_reset(GetTransactionFromArgs(info));
}

void Transaction::Cancel(const Nan::FunctionCallbackInfo<Value>& info) {
  fdb_transaction_cancel(GetTransactionFromArgs(info));
}

// See fdb_transaction_on_error documentation to see how to handle this.
// This is all wrapped by JS.
void Transaction::OnError(const Nan::FunctionCallbackInfo<Value>& info) {
  fdb_error_t errorCode = info[0]->Int32Value();
  FDBFuture *f = fdb_transaction_on_error(GetTransactionFromArgs(info), errorCode);
  info.GetReturnValue().Set(futureToJS(f, info[1], ignoreResult));
}



// Get(key, isSnapshot, [cb])
void Transaction::Get(const Nan::FunctionCallbackInfo<Value>& info) {
  StringParams key(info[0]);
  bool snapshot = info[1]->BooleanValue();

  FDBFuture *f = fdb_transaction_get(GetTransactionFromArgs(info), key.str, key.len, snapshot);

  info.GetReturnValue().Set(futureToJS(f, info[2], getValue));
}

/*
 * This function takes a KeySelector and returns a future.
 */
// GetKey(key, selOrEq, offset, isSnapshot, [cb])
void Transaction::GetKey(const Nan::FunctionCallbackInfo<Value>& info) {
  StringParams key(info[0]);
  int selectorOrEqual = info[1]->Int32Value();
  int selectorOffset = info[2]->Int32Value();
  bool snapshot = info[3]->BooleanValue();

  FDBFuture *f = fdb_transaction_get_key(GetTransactionFromArgs(info), key.str, key.len, (fdb_bool_t)selectorOrEqual, selectorOffset, snapshot);

  info.GetReturnValue().Set(futureToJS(f, info[4], getKey));
}

// set(key, val). Syncronous.
void Transaction::Set(const Nan::FunctionCallbackInfo<Value>& info){
  StringParams key(info[0]);
  StringParams val(info[1]);
  fdb_transaction_set(GetTransactionFromArgs(info), key.str, key.len, val.str, val.len);
}

// Delete value stored for key.
// clear("somekey")
void Transaction::Clear(const Nan::FunctionCallbackInfo<Value>& info) {
  StringParams key(info[0]);
  fdb_transaction_clear(GetTransactionFromArgs(info), key.str, key.len);
}

// atomicOp(key, operand key, mutationtype)
void Transaction::AtomicOp(const Nan::FunctionCallbackInfo<Value>& info) {
  StringParams key(info[0]);
  StringParams operand(info[1]);
  FDBMutationType operationType = (FDBMutationType)info[2]->Int32Value();

  fdb_transaction_atomic_op(GetTransactionFromArgs(info), key.str, key.len, operand.str, operand.len, operationType);
}

// getRange(
//   start, beginOrEqual, beginOffset,
//   end, endOrEqual, endOffset,
//   limit or 0, target_bytes or 0,
//   streamingMode, iteration,
//   snapshot, reverse,
//   [cb]
// )
void Transaction::GetRange(const Nan::FunctionCallbackInfo<Value>& info) {
  StringParams start(info[0]);
  int startOrEqual = info[1]->BooleanValue();
  int startOffset = info[2]->Int32Value();

  StringParams end(info[3]);
  int endOrEqual = info[4]->BooleanValue();
  int endOffset = info[5]->Int32Value();

  int limit = info[6]->Int32Value();
  int target_bytes = info[7]->Int32Value();
  FDBStreamingMode mode = (FDBStreamingMode)info[8]->Int32Value();
  int iteration = info[9]->Int32Value();
  bool snapshot = info[10]->BooleanValue();
  bool reverse = info[11]->BooleanValue();

  FDBFuture *f = fdb_transaction_get_range(GetTransactionFromArgs(info),
    start.str, start.len, (fdb_bool_t)startOrEqual, startOffset,
    end.str, end.len, (fdb_bool_t)endOrEqual, endOffset,
    limit, target_bytes,
    mode, iteration,
    snapshot, reverse);

  info.GetReturnValue().Set(futureToJS(f, info[12], getKeyValueList));
}



// clearRange(start, end, [cb]). Clears range [start, end).
void Transaction::ClearRange(const Nan::FunctionCallbackInfo<Value>& info) {
  StringParams begin(info[0]);
  StringParams end(info[1]);
  fdb_transaction_clear_range(GetTransactionFromArgs(info), begin.str, begin.len, end.str, end.len);
}






void Transaction::SetReadVersion(const Nan::FunctionCallbackInfo<Value>& info) {
  // TODO: Support info[0] being an opaque buffer.
  int64_t version = info[0]->IntegerValue();
  fdb_transaction_set_read_version(GetTransactionFromArgs(info), version);
}

void Transaction::GetReadVersion(const Nan::FunctionCallbackInfo<Value>& info) {
  FDBFuture *f = fdb_transaction_get_read_version(GetTransactionFromArgs(info));
  info.GetReturnValue().Set(futureToJS(f, info[0], getVersion));
}

void Transaction::GetCommittedVersion(const Nan::FunctionCallbackInfo<Value>& info) {
  int64_t version;
  fdb_error_t errorCode = fdb_transaction_get_committed_version(GetTransactionFromArgs(info), &version);

  if(errorCode != 0) {
    return Nan::ThrowError(FdbError::NewInstance(errorCode));
  }

  // Again, if we change version to be a byte array this will need to change too.
  info.GetReturnValue().Set((double)version);
}


void Transaction::GetVersionStamp(const Nan::FunctionCallbackInfo<Value>& info) {
  FDBFuture *f = fdb_transaction_get_read_version(GetTransactionFromArgs(info));
  info.GetReturnValue().Set(futureToJS(f, info[0], getKey));
}

// getAddressesForKey("somekey", [cb])
void Transaction::GetAddressesForKey(const Nan::FunctionCallbackInfo<Value>& info) {
  StringParams key(info[0]);

  FDBFuture *f = fdb_transaction_get_addresses_for_key(GetTransactionFromArgs(info), key.str, key.len);
  info.GetReturnValue().Set(futureToJS(f, info[1], getStringArray));
}






// Not exposed to JS. Simple wrapper. Call AddReadConflictRange / AddWriteConflictRange.
void Transaction::AddConflictRange(const Nan::FunctionCallbackInfo<Value>& info, FDBConflictRangeType type) {
  StringParams start(info[0]);
  StringParams end(info[1]);

  fdb_error_t errorCode = fdb_transaction_add_conflict_range(GetTransactionFromArgs(info), start.str, start.len, end.str, end.len, type);

  if(errorCode) Nan::ThrowError(FdbError::NewInstance(errorCode));
}

// addConflictRange(start, end)
void Transaction::AddReadConflictRange(const Nan::FunctionCallbackInfo<Value>& info) {
  return AddConflictRange(info, FDB_CONFLICT_RANGE_TYPE_READ);
}

// addConflictRange(start, end)
void Transaction::AddWriteConflictRange(const Nan::FunctionCallbackInfo<Value>& info) {
  return AddConflictRange(info, FDB_CONFLICT_RANGE_TYPE_WRITE);
}









void Transaction::New(const Nan::FunctionCallbackInfo<Value>& info) {
  Transaction *tr = new Transaction();
  tr->Wrap(info.Holder());
}

Local<Value> Transaction::NewInstance(FDBTransaction *ptr) {
  Isolate *isolate = Isolate::GetCurrent();
  Nan::EscapableHandleScope scope;

  Local<Function> transactionConstructor = Local<Function>::New(isolate, constructor);
  Local<Object> instance = Nan::NewInstance(transactionConstructor).ToLocalChecked();

  Transaction *trObj = ObjectWrap::Unwrap<Transaction>(instance);
  trObj->tr = ptr;

  // instance->Set(String::NewFromUtf8(isolate, "options", String::kInternalizedString), FdbOptions::CreateOptions(FdbOptions::TransactionOption, instance));

  return scope.Escape(instance);
}

void Transaction::Init() {
  Local<FunctionTemplate> tpl = Nan::New<FunctionTemplate>(New);

  tpl->SetClassName(Nan::New<v8::String>("Transaction").ToLocalChecked());
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  Nan::SetPrototypeMethod(tpl, "setOption", SetOption);

  Nan::SetPrototypeMethod(tpl, "commit", Commit);
  Nan::SetPrototypeMethod(tpl, "reset", Reset);
  Nan::SetPrototypeMethod(tpl, "onError", OnError);

  Nan::SetPrototypeMethod(tpl, "get", Get);
  Nan::SetPrototypeMethod(tpl, "getRange", GetRange);
  Nan::SetPrototypeMethod(tpl, "getKey", GetKey);
  Nan::SetPrototypeMethod(tpl, "watch", Watch);
  Nan::SetPrototypeMethod(tpl, "set", Set);
  Nan::SetPrototypeMethod(tpl, "clear", Clear);
  Nan::SetPrototypeMethod(tpl, "clearRange", ClearRange);
  Nan::SetPrototypeMethod(tpl, "addReadConflictRange", AddReadConflictRange);
  Nan::SetPrototypeMethod(tpl, "addWriteConflictRange", AddWriteConflictRange);
  Nan::SetPrototypeMethod(tpl, "getReadVersion", GetReadVersion);
  Nan::SetPrototypeMethod(tpl, "setReadVersion", SetReadVersion);
  Nan::SetPrototypeMethod(tpl, "getCommittedVersion", GetCommittedVersion);
  Nan::SetPrototypeMethod(tpl, "cancel", Cancel);
  Nan::SetPrototypeMethod(tpl, "getAddressesForKey", GetAddressesForKey);

  constructor.Reset(tpl->GetFunction());
}




// Gross clean me up!
struct NodeVoidCallback : NodeCallback {

  NodeVoidCallback(FDBFuture *future, Local<Function> cbFunc) : NodeCallback(future, cbFunc) { }

  virtual Local<Value> extractValue(FDBFuture* future, fdb_error_t& outErr) {
    Isolate *isolate = Isolate::GetCurrent();
    outErr = fdb_future_get_error(future);
    return Undefined(isolate);
  }
};

// watch("somekey", listener) -> {cancel()}. This does not return a promise.
// Due to race conditions the listener may be called even after cancel has been called.
//
// TODO: Move this over to the new infrastructure.
void Transaction::Watch(const Nan::FunctionCallbackInfo<Value>& info) {
  StringParams key(info[0]);

  Isolate *isolate = Isolate::GetCurrent();
  FDBTransaction *tr = GetTransactionFromArgs(info);

  Local<Function> cb = Local<Function>::New(isolate, Local<Function>::Cast(info[1]));

  FDBFuture *f = fdb_transaction_watch(tr, key.str, key.len);
  NodeVoidCallback *callback = new NodeVoidCallback(f, cb);
  Local<Value> watch = Watch::NewInstance(callback);

  callback->start();
  info.GetReturnValue().Set(watch);
}




// Watch implementation
Watch::Watch() : callback(NULL) { };

Watch::~Watch() {
  if(callback) {
    if(callback->getFuture())
      fdb_future_cancel(callback->getFuture());

    callback->delRef();
  }
};

Nan::Persistent<Function> Watch::constructor;

Local<Value> Watch::NewInstance(NodeCallback *callback) {
  Isolate *isolate = Isolate::GetCurrent();
  Nan::EscapableHandleScope scope;

  Local<Function> watchConstructor = Local<Function>::New(isolate, constructor);
  Local<Object> instance = Nan::NewInstance(watchConstructor).ToLocalChecked();

  Watch *watchObj = ObjectWrap::Unwrap<Watch>(instance);
  watchObj->callback = callback;
  callback->addRef();

  return scope.Escape(instance);
}

void Watch::New(const Nan::FunctionCallbackInfo<Value>& info) {
  Watch *c = new Watch();
  c->Wrap(info.Holder());
}

void Watch::Cancel(const Nan::FunctionCallbackInfo<Value>& info) {
  NodeCallback *callback = node::ObjectWrap::Unwrap<Watch>(info.Holder())->callback;

  if(callback && callback->getFuture()) {
    fdb_future_cancel(callback->getFuture());
  }
}

void Watch::Init() {
  Local<FunctionTemplate> tpl = Nan::New<FunctionTemplate>(New);

  tpl->SetClassName(Nan::New<v8::String>("Watch").ToLocalChecked());
  tpl->InstanceTemplate()->SetInternalFieldCount(1);

  Nan::SetPrototypeMethod(tpl, "cancel", Cancel);

  constructor.Reset(tpl->GetFunction());
}
