// Stuff that hasn't been ported over:

// const Transactional = require('./retryDecorator')
// const locality = require('./locality')
// const directory = require('./directory')

import nativeMod, * as fdb from './native'
import Database from './database'
import {eachOption} from './opts'
import {NetworkOptions, networkOptionData, DatabaseOptions} from './opts.g'
import {Transformer} from './transformer'
import {defaultSubspace} from './subspace'
import {DirectoryLayer} from './directory'

import * as apiVersion from './apiVersion'

import {deprecate} from 'util'

// Must be called before fdb is initialized. Eg setAPIVersion(510).
export {set as setAPIVersion} from './apiVersion'

// 'napi'
export const modType = fdb.type

let initCalled = false

// This is called implicitly when the first cluster / db is opened.
const init = () => {
  if (apiVersion.get() == null) {
    throw Error('You must specify an API version to connect to FoundationDB. Eg: fdb.setAPIVersion(510);')
  }

  if (initCalled) return
  initCalled = true

  nativeMod.startNetwork()

  process.on('exit', () => nativeMod.stopNetwork())
}

// Destroy the network thread. This is not needed under normal circumstances;
// but can be used to de-init FDB.
export const stopNetworkSync = nativeMod.stopNetwork

export {default as FDBError} from './error'
export {default as keySelector, KeySelector} from './keySelector'

// These are exported to give consumers access to the type. Databases must
// always be constructed using open or via a cluster object.
export {default as Database} from './database'
export {default as Transaction, Watch} from './transaction'
export {default as Subspace, defaultSubspace} from './subspace'
export {Directory, DirectoryLayer} from './directory'

export const directory = new DirectoryLayer() // Convenient root directory

export {
  NetworkOptions,
  NetworkOptionCode,
  DatabaseOptions,
  DatabaseOptionCode,
  TransactionOptions,
  TransactionOptionCode,
  StreamingMode,
  MutationType,
  ConflictRangeType,
  ErrorPredicate,
} from './opts.g'

import {strInc} from './util'
export const util = {strInc}

// TODO: Remove tuple from the root API. Tuples should be in a separate module.
import * as tuple from './tuple'
import {TupleItem} from './tuple'
// import * as tuple from './tuple'

export {TupleItem, tuple}


const id = (x: any) => x
export const encoders = {
  int32BE: {
    pack(num) {
      const b = Buffer.alloc(4)
      b.writeInt32BE(num, 0)
      return b
    },
    unpack(buf) { return buf.readInt32BE(0) }
  } as Transformer<number, number>,

  json: {
    pack(obj) { return JSON.stringify(obj) },
    unpack(buf) { return JSON.parse(buf.toString('utf8')) }
  } as Transformer<any, any>,

  string: {
    pack(str) { return Buffer.from(str, 'utf8') },
    unpack(buf) { return buf.toString('utf8') }
  } as Transformer<string, string>,

  buf: {
    pack: id,
    unpack: id
  } as Transformer<Buffer, Buffer>,

  // TODO: Move this into a separate library
  tuple: tuple as Transformer<TupleItem[], TupleItem[]>,
}

// Can only be called before open() or openSync().
export function configNetwork(netOpts: NetworkOptions) {
  if (initCalled) throw Error('configNetwork must be called before FDB connections are opened')
  eachOption(networkOptionData, netOpts, (code, val) => nativeMod.setNetworkOption(code, val))
}

/**
 * Opens a database and returns it.
 *
 * Note any network configuration must happen before the database is opened.
 */
export function open(clusterFile?: string, dbOpts?: DatabaseOptions) {
  init()

  const db = new Database(nativeMod.createDatabase(clusterFile), defaultSubspace)
  if (dbOpts) db.setNativeOptions(dbOpts)
  
  // Users of the previous version of the API would expect the object returned
  // here to be a promise. I'm still breaking backwards compatibility by
  // returning synchronously now but ...
  ;(db as any).next = deprecate((fn: any) => {
    setImmediate(fn, db)
  }, 'fdb.open() returns a database synchronously. No need to use open().next(...)')

  return db
}

// *** Some deprecated stuff to remove:

/** @deprecated Async database connection has been removed from FDB. Call open() directly. */
export const openSync = deprecate(open, 'Async database connection has been removed from FDB. Call open() directly.')

// Previous versions of this library allowed you to create a cluster and then
// create database objects from it. This was all removed from the C API. We'll
// fake it for now, and remove this later.

const stubCluster = (clusterFile?: string) => ({
  openDatabase(dbName: 'DB' = 'DB', opts?: DatabaseOptions) {
    return Promise.resolve(open(clusterFile, opts))
  },
  openDatabaseSync(dbName: 'DB' = 'DB', opts?: DatabaseOptions) {
    return open(clusterFile, opts)
  },
  close() {}
})

/** @deprecated FDB clusters have been removed from the API. Call open() directly to connect. */
export const createCluster = deprecate((clusterFile?: string) => {
  return Promise.resolve(stubCluster(clusterFile))
}, 'FDB clusters have been removed from the API. Call open() directly to connect.')

/** @deprecated FDB clusters have been removed from the API. Call open() directly to connect. */
export const createClusterSync = deprecate((clusterFile?: string) => {
  return stubCluster(clusterFile)
}, 'FDB clusters have been removed from the API. Call open() directly to connect.')


// TODO: Should I expose a method here for stopping the network for clean shutdown?
// I feel like I should.. but I'm not sure when its useful. Will the network thread
// keep the process running?