// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

library pub.system_cache;

import 'dart:async';

import 'package:path/path.dart' as path;

import 'io.dart';
import 'io.dart' as io show createTempDir;
import 'log.dart' as log;
import 'package.dart';
import 'source/git.dart';
import 'source/hosted.dart';
import 'source/path.dart';
import 'source.dart';
import 'source_registry.dart';

/// The system-wide cache of downloaded packages.
///
/// This cache contains all packages that are downloaded from the internet.
/// Packages that are available locally (e.g. path dependencies) don't use this
/// cache.
class SystemCache {
  /// The root directory where this package cache is located.
  final String rootDir;

  String get tempDir => path.join(rootDir, '_temp');

  /// Packages which are currently being asynchronously downloaded to the cache.
  final Map<PackageId, Future<Package>> _pendingDownloads;

  /// The sources from which to get packages.
  final SourceRegistry sources;

  /// Creates a new package cache which is backed by the given directory on the
  /// user's file system.
  SystemCache(this.rootDir)
  : _pendingDownloads = new Map<PackageId, Future<Package>>(),
    sources = new SourceRegistry();

  /// Creates a system cache and registers the standard set of sources. If
  /// [isOffline] is `true`, then the offline hosted source will be used.
  /// Defaults to `false`.
  factory SystemCache.withSources(String rootDir, {bool isOffline: false}) {
    var cache = new SystemCache(rootDir);
    cache.register(new GitSource());

    if (isOffline) {
      cache.register(new OfflineHostedSource());
    } else {
      cache.register(new HostedSource());
    }

    cache.register(new PathSource());
    cache.sources.setDefault('hosted');
    return cache;
  }

  /// Registers a new source. This source must not have the same name as a
  /// source that's already been registered.
  void register(Source source) {
    source.bind(this);
    sources.register(source);
  }

  /// Ensures that the package identified by [id] is downloaded to the cache,
  /// loads it, and returns it.
  ///
  /// It is an error to try downloading a package from a source with
  /// `shouldCache == false`.
  Future<Package> download(PackageId id) {
    var source = sources[id.source];

    if (!source.shouldCache) {
      throw new ArgumentError("Package $id is not cacheable.");
    }

    var pending = _pendingDownloads[id];
    if (pending != null) return pending;

    var future = source.downloadToSystemCache(id).whenComplete(() {
      _pendingDownloads.remove(id);
    });

    _pendingDownloads[id] = future;
    return future;
  }

  /// Create a new temporary directory within the system cache. The system
  /// cache maintains its own temporary directory that it uses to stage
  /// packages into while downloading. It uses this instead of the OS's system
  /// temp directory to ensure that it's on the same volume as the pub system
  /// cache so that it can move the directory from it.
  String createTempDir() {
    var temp = ensureDir(tempDir);
    return io.createTempDir(temp, 'dir');
  }

  /// Deletes the system cache's internal temp directory.
  void deleteTempDir() {
    log.fine('Clean up system cache temp directory $tempDir.');
    if (dirExists(tempDir)) deleteEntry(tempDir);
  }
}
