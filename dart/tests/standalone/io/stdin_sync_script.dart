// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import "dart:convert";
import "dart:io";
import "dart:json";

void main() {
  var arguments = new Options().arguments;
  int i = 0;
  String line;
  while ((line = stdin.readLineSync(encoding: UTF8)) != null) {
    if (parse(arguments[i]) != line) throw "bad line at $i: ${line.codeUnits}";
    i++;
  }
  if (i != arguments.length) throw "expect ${arguments.length} lines";
  print('true');
}
