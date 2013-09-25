// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

part of html;

class _ConsoleVariables {
  Map<String, Object> _data = new Map<String, Object>();

  /**
   * Forward member accesses to the backing JavaScript object.
   */
  noSuchMethod(Invocation invocation) {
    String member = MirrorSystem.getName(invocation.memberName);
    if (invocation.isGetter) {
      return _data[member];
    } else if (invocation.isSetter) {
      assert(member.endsWith('='));
      member = member.substring(0, member.length - 1);
      _data[member] = invocation.positionalArguments[0];
    } else {
      return Function.apply(_data[member], invocation.positionalArguments, invocation.namedArguments);
    }
  }

  void clear() => _data.clear();

  /**
   * List all variables currently defined.
   */
  List variables() => _data.keys.toList(growable: false);
}

class _Utils {
  static double dateTimeToDouble(DateTime dateTime) =>
      dateTime.millisecondsSinceEpoch.toDouble();
  static DateTime doubleToDateTime(double dateTime) {
    try {
      return new DateTime.fromMillisecondsSinceEpoch(dateTime.toInt());
    } catch(_) {
      // TODO(antonnm): treat exceptions properly in bindings and
      // find out how to treat NaNs.
      return null;
    }
  }

  static List convertToList(List list) {
    // FIXME: [possible optimization]: do not copy the array if Dart_IsArray is fine w/ it.
    final length = list.length;
    List result = new List(length);
    result.setRange(0, length, list);
    return result;
  }

  static List convertMapToList(Map map) {
    List result = [];
    map.forEach((k, v) => result.addAll([k, v]));
    return result;
  }

  static int convertCanvasElementGetContextMap(Map map) {
    int result = 0;
    if (map['alpha'] == true) result |= 0x01;
    if (map['depth'] == true) result |= 0x02;
    if (map['stencil'] == true) result |= 0x4;
    if (map['antialias'] == true) result |= 0x08;
    if (map['premultipliedAlpha'] == true) result |= 0x10;
    if (map['preserveDrawingBuffer'] == true) result |= 0x20;

    return result;
  }

  static List parseStackTrace(StackTrace stackTrace) {
    final regExp = new RegExp(r'#\d\s+(.*) \((.*):(\d+):(\d+)\)');
    List result = [];
    for (var match in regExp.allMatches(stackTrace.toString())) {
      result.add([match.group(1), match.group(2), int.parse(match.group(3)), int.parse(match.group(4))]);
    }
    return result;
  }

  static void populateMap(Map result, List list) {
    for (int i = 0; i < list.length; i += 2) {
      result[list[i]] = list[i + 1];
    }
  }

  static bool isMap(obj) => obj is Map;

  static Map createMap() => {};

  static makeUnimplementedError(String fileName, int lineNo) {
    return new UnsupportedError('[info: $fileName:$lineNo]');
  }

  static bool isTypeSubclassOf(Type type, Type other) {
    if (type == other) {
      return true;
    }
    var superclass = reflectClass(type).superclass;
    if (superclass != null) {
      return isTypeSubclassOf(superclass.reflectedType, other);
    }
    return false;
  }

  static window() native "Utils_window";
  static forwardingPrint(String message) native "Utils_forwardingPrint";
  static void spawnDomFunction(Function f, int replyTo) native "Utils_spawnDomFunction";
  static int _getNewIsolateId() native "Utils_getNewIsolateId";

  // The following methods were added for debugger integration to make working
  // with the Dart C mirrors API simpler.
  // TODO(jacobr): consider moving them to a separate library.
  // If Dart supported dynamic code injection, we would only inject this code
  // when the debugger is invoked.

  /**
   * Strips the private secret prefix from member names of the form
   * someName@hash.
   */
  static String stripMemberName(String name) {
    int endIndex = name.indexOf('@');
    return endIndex > 0 ? name.substring(0, endIndex) : name;
  }

  /**
   * Takes a list containing variable names and corresponding values and
   * returns a map from normalized names to values. Variable names are assumed
   * to have list offsets 2*n values at offset 2*n+1. This method is required
   * because Dart_GetLocalVariables returns a list instead of an object that
   * can be queried to lookup names and values.
   */
  static Map<String, dynamic> createLocalVariablesMap(List localVariables) {
    var map = {};
    for (int i = 0; i < localVariables.length; i+=2) {
      map[stripMemberName(localVariables[i])] = localVariables[i+1];
    }
    return map;
  }

  static _ConsoleVariables _consoleTempVariables = new _ConsoleVariables();
  /**
   * Takes an [expression] and a list of [local] variable and returns an
   * expression for a closure with a body matching the original expression
   * where locals are passed in as arguments. Returns a list containing the
   * String expression for the closure and the list of arguments that should
   * be passed to it. The expression should then be evaluated using
   * Dart_EvaluateExpr which will generate a closure that should be invoked
   * with the list of arguments passed to this method.
   *
   * For example:
   * <code>wrapExpressionAsClosure("foo + bar", ["bar", 40, "foo", 2])</code>
   * will return:
   * <code>["(final $var, final bar, final foo) => foo + bar", [40, 2]]</code>
   */
  static List wrapExpressionAsClosure(String expression, List locals) {
    var args = {};
    var sb = new StringBuffer("(");
    addArg(arg, value) {
      arg = stripMemberName(arg);
      if (args.containsKey(arg)) return;
      // We ignore arguments with the name 'this' rather than throwing an
      // exception because Dart_GetLocalVariables includes 'this' and it
      // is more convenient to filter it out here than from C++ code.
      // 'this' needs to be handled by calling Dart_EvaluateExpr with
      // 'this' as the target rather than by passing it as an argument.
      if (arg == 'this') return;
      if (args.isNotEmpty) {
        sb.write(", ");
      }
      sb.write("final $arg");
      args[arg] = value;
    }

    addArg("\$var", _consoleTempVariables);

    for (int i = 0; i < locals.length; i+= 2) {
      addArg(locals[i], locals[i+1]);
    }
    sb..write(')=>\n$expression');
    return [sb.toString(), args.values.toList(growable: false)];
  }

  /**
   * Convenience helper to get the keys of a [Map] as a [List].
   */
  static List getMapKeyList(Map map) => map.keys.toList();

 /**
   * Returns the keys of an arbitrary Dart Map encoded as unique Strings.
   * Keys that are strings are left unchanged except that the prefix ":" is
   * added to disambiguate keys from other Dart members.
   * Keys that are not strings have # followed by the index of the key in the map
   * prepended to disambuguate. This scheme is simplistic but easy to encode and
   * decode. The use case for this method is displaying all map keys in a human
   * readable way in debugging tools.
   */
  static List<String> getEncodedMapKeyList(dynamic obj) {
    if (obj is! Map) return null;

    var ret = new List<String>();
    int i = 0;
    return obj.keys.map((key) {
      var encodedKey;
      if (key is String) {
        encodedKey = ':$key';
      } else {
        // If the key isn't a string, return a guaranteed unique for this map
        // string representation of the key that is still somewhat human
        // readable.
        encodedKey = '#${i}:$key';
      }
      i++;
      return encodedKey;
    }).toList(growable: false);
  }

  static final RegExp _NON_STRING_KEY_REGEXP = new RegExp("^#(\\d+):(.+)\$");

  static _decodeKey(Map map, String key) {
    // The key is a regular old String.
    if (key.startsWith(':')) return key.substring(1);

    var match = _NON_STRING_KEY_REGEXP.firstMatch(key);
    if (match != null) {
      int index = int.parse(match.group(1));
      var iter = map.keys.skip(index);
      if (iter.isNotEmpty) {
        var ret = iter.first;
        // Validate that the toString representation of the key matches what we
        // expect. FIXME: throw an error if it does not.
        assert(match.group(2) == '$ret');
        return ret;
      }
    }
    return null;
  }

  /**
   * Converts keys encoded with [getEncodedMapKeyList] to their actual keys.
   */
  static lookupValueForEncodedMapKey(Map obj, String key) => obj[_decodeKey(obj, key)];

  /**
   * Builds a constructor name with the form expected by the C Dart mirrors API.
   */
  static String buildConstructorName(String className, String constructorName) => '$className.$constructorName';

  /**
   * Strips the class name from an expression of the form "className.someName".
   */
  static String stripClassName(String str, String className) {
    if (str.length > className.length + 1 &&
        str.startsWith(className) && str[className.length] == '.') {
      return str.substring(className.length + 1);
    } else {
      return str;
    }
  }

  /**
   * Removes the trailing dot from an expression ending in a dot.
   * This method is used as Library prefixes include a trailing dot when using
   * the C Dart debugger API.
   */
  static String stripTrailingDot(String str) =>
    (str != null && str[str.length - 1] == '.') ? str.substring(0, str.length - 1) : str;

  static String addTrailingDot(String str) => '${str}.';

  static bool isNoSuchMethodError(obj) => obj is NoSuchMethodError;

  // TODO(jacobr): we need a failsafe way to determine that a Node is really a
  // DOM node rather than just a class that extends Node.
  static bool isNode(obj) => obj is Node;

  static bool _isBuiltinType(ClassMirror cls) {
    // TODO(vsm): Find a less hackish way to do this.
    LibraryMirror lib = cls.owner;
    String libName = lib.uri.toString();
    return libName.startsWith('dart:');
  }

  static void register(Document document, String tag, Type type,
      String extendsTagName) {
    // TODO(vsm): Move these checks into native code.
    ClassMirror cls = reflectClass(type);
    if (_isBuiltinType(cls)) {
      throw new UnsupportedError("Invalid custom element from $libName.");
    }
    _register(document, tag, type, extendsTagName);
  }

  static void _register(Document document, String tag, Type customType,
      String extendsTagName) native "Utils_register";

  static Element createElement(Document document, String tagName) native "Utils_createElement";
}

class _NPObject extends NativeFieldWrapperClass1 {
  _NPObject.internal();
  static _NPObject retrieve(String key) native "NPObject_retrieve";
  property(String propertyName) native "NPObject_property";
  invoke(String methodName, [List args = null]) native "NPObject_invoke";
}

class _DOMWindowCrossFrame extends NativeFieldWrapperClass1 implements
    WindowBase {
  _DOMWindowCrossFrame.internal();

  // Fields.
  HistoryBase get history native "Window_history_cross_frame_Getter";
  LocationBase get location native "Window_location_cross_frame_Getter";
  bool get closed native "Window_closed_Getter";
  int get length native "Window_length_Getter";
  WindowBase get opener native "Window_opener_Getter";
  WindowBase get parent native "Window_parent_Getter";
  WindowBase get top native "Window_top_Getter";

  // Methods.
  void close() native "Window_close_Callback";
  void postMessage(/*SerializedScriptValue*/ message, String targetOrigin, [List messagePorts]) native "Window_postMessage_Callback";

  // Implementation support.
  String get typeName => "Window";
}

class _HistoryCrossFrame extends NativeFieldWrapperClass1 implements HistoryBase {
  _HistoryCrossFrame.internal();

  // Methods.
  void back() native "History_back_Callback";
  void forward() native "History_forward_Callback";
  void go(int distance) native "History_go_Callback";

  // Implementation support.
  String get typeName => "History";
}

class _LocationCrossFrame extends NativeFieldWrapperClass1 implements LocationBase {
  _LocationCrossFrame.internal();

  // Fields.
  void set href(String) native "Location_href_Setter";

  // Implementation support.
  String get typeName => "Location";
}

class _DOMStringMap extends NativeFieldWrapperClass1 implements Map<String, String> {
  _DOMStringMap.internal();

  bool containsValue(String value) => Maps.containsValue(this, value);
  bool containsKey(String key) native "DOMStringMap_containsKey_Callback";
  String operator [](String key) native "DOMStringMap_item_Callback";
  void operator []=(String key, String value) native "DOMStringMap_setItem_Callback";
  String putIfAbsent(String key, String ifAbsent()) => Maps.putIfAbsent(this, key, ifAbsent);
  String remove(String key) native "DOMStringMap_remove_Callback";
  void clear() => Maps.clear(this);
  void forEach(void f(String key, String value)) => Maps.forEach(this, f);
  Iterable<String> get keys native "DOMStringMap_getKeys_Callback";
  Iterable<String> get values => Maps.getValues(this);
  int get length => Maps.length(this);
  bool get isEmpty => Maps.isEmpty(this);
  bool get isNotEmpty => Maps.isNotEmpty(this);
}

// TODO(vsm): Remove DOM isolate code.  This is only used to support
// printing and timers in background isolates.  Background isolates
// should just forward to the main DOM isolate instead of requiring a
// special DOM isolate.

_makeSendPortFuture(spawnRequest) {
  final completer = new Completer<SendPort>.sync();
  final port = new ReceivePort();
  port.receive((result, _) {
    completer.complete(result);
    port.close();
  });
  // TODO: SendPort.hashCode is ugly way to access port id.
  spawnRequest(port.toSendPort().hashCode);
  return completer.future;
}

Future<SendPort> _spawnDomFunction(Function f) =>
    _makeSendPortFuture((portId) { _Utils.spawnDomFunction(f, portId); });

final Future<SendPort> __HELPER_ISOLATE_PORT =
    _spawnDomFunction(_helperIsolateMain);

// Tricky part.
// Once __HELPER_ISOLATE_PORT gets resolved, it will still delay in .then
// and to delay Timer.run is used. However, Timer.run will try to register
// another Timer and here we got stuck: event cannot be posted as then
// callback is not executed because it's delayed with timer.
// Therefore once future is resolved, it's unsafe to call .then on it
// in Timer code.
SendPort __SEND_PORT;

_sendToHelperIsolate(msg, SendPort replyTo) {
  if (__SEND_PORT != null) {
    __SEND_PORT.send(msg, replyTo);
  } else {
    __HELPER_ISOLATE_PORT.then((port) {
      __SEND_PORT = port;
      __SEND_PORT.send(msg, replyTo);
    });
  }
}

final _TIMER_REGISTRY = new Map<SendPort, Timer>();

const _NEW_TIMER = 'NEW_TIMER';
const _CANCEL_TIMER = 'CANCEL_TIMER';
const _TIMER_PING = 'TIMER_PING';
const _PRINT = 'PRINT';

_helperIsolateMain() {
  port.receive((msg, replyTo) {
    final cmd = msg[0];
    if (cmd == _NEW_TIMER) {
      final duration = new Duration(milliseconds: msg[1]);
      bool periodic = msg[2];
      ping() { replyTo.send(_TIMER_PING); };
      _TIMER_REGISTRY[replyTo] = periodic ?
          new Timer.periodic(duration, (_) { ping(); }) :
          new Timer(duration, ping);
    } else if (cmd == _CANCEL_TIMER) {
      _TIMER_REGISTRY.remove(replyTo).cancel();
    } else if (cmd == _PRINT) {
      final message = msg[1];
      // TODO(antonm): we need somehow identify those isolates.
      print('[From isolate] $message');
    }
  });
}

final _printClosure = window.console.log;
final _pureIsolatePrintClosure = (s) {
  _sendToHelperIsolate([_PRINT, s], null);
};

final _forwardingPrintClosure = _Utils.forwardingPrint;

 class _Timer implements Timer {
  var _canceler;

  _Timer(int milliSeconds, void callback(Timer timer), bool repeating) {

    if (repeating) {
      int id = window._setInterval(() {
        callback(this);
      }, milliSeconds);
      _canceler = () => window._clearInterval(id);
    } else {
      int id = window._setTimeout(() {
        _canceler = null;
        callback(this);
      }, milliSeconds);
      _canceler = () => window._clearTimeout(id);
    }
  }

  void cancel() {
    if (_canceler != null) {
      _canceler();
    }
    _canceler = null;
  }

  bool get isActive => _canceler != null;
}

get _timerFactoryClosure =>
    (int milliSeconds, void callback(Timer timer), bool repeating) {
  return new _Timer(milliSeconds, callback, repeating);
};


class _PureIsolateTimer implements Timer {
  bool _isActive = true;
  final ReceivePort _port = new ReceivePort();
  SendPort _sendPort; // Effectively final.

  static SendPort _SEND_PORT;

  _PureIsolateTimer(int milliSeconds, callback, repeating) {
    _sendPort = _port.toSendPort();
    _port.receive((msg, replyTo) {
      assert(msg == _TIMER_PING);
      _isActive = repeating;
      callback(this);
      if (!repeating) _cancel();
    });

    _send([_NEW_TIMER, milliSeconds, repeating]);
  }

  void cancel() {
    _cancel();
    _send([_CANCEL_TIMER]);
  }

  void _cancel() {
    _isActive = false;
    _port.close();
  }

  _send(msg) {
    _sendToHelperIsolate(msg, _sendPort);
  }

  bool get isActive => _isActive;
}

get _pureIsolateTimerFactoryClosure =>
    ((int milliSeconds, void callback(Timer time), bool repeating) =>
        new _PureIsolateTimer(milliSeconds, callback, repeating));
