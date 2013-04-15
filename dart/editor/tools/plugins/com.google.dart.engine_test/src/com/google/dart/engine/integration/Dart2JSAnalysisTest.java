/*
 * Copyright (c) 2013, the Dart project authors.
 * 
 * Licensed under the Eclipse Public License v1.0 (the "License"); you may not use this file except
 * in compliance with the License. You may obtain a copy of the License at
 * 
 * http://www.eclipse.org/legal/epl-v10.html
 * 
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied. See the License for the specific language governing permissions and limitations under
 * the License.
 */
package com.google.dart.engine.integration;

import com.google.dart.engine.AnalysisEngine;
import com.google.dart.engine.ast.CompilationUnit;
import com.google.dart.engine.context.AnalysisContext;
import com.google.dart.engine.context.AnalysisException;
import com.google.dart.engine.element.CompilationUnitElement;
import com.google.dart.engine.element.LibraryElement;
import com.google.dart.engine.error.AnalysisError;
import com.google.dart.engine.error.ErrorCode;
import com.google.dart.engine.internal.resolver.ResolutionVerifier;
import com.google.dart.engine.internal.resolver.StaticTypeVerifier;
import com.google.dart.engine.sdk.DartSdk;
import com.google.dart.engine.sdk.DirectoryBasedDartSdk;
import com.google.dart.engine.source.DartUriResolver;
import com.google.dart.engine.source.FileBasedSource;
import com.google.dart.engine.source.FileUriResolver;
import com.google.dart.engine.source.Source;
import com.google.dart.engine.source.SourceFactory;
import com.google.dart.engine.utilities.io.PrintStringWriter;

import junit.framework.Assert;
import junit.framework.TestCase;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;

public class Dart2JSAnalysisTest extends TestCase {

  private static String[] DART2JS_LIB_PATHS = {
      "dart2js.dart", "apiimpl.dart", "closure.dart", "colors.dart", "dart_types.dart",
      "deferred_load.dart", "filenames.dart", "native_handler.dart", "patch_parser.dart",
      "source_file.dart", "source_file_provider.dart", "source_map_builder.dart",
      "string_validator.dart", "dart_backend/dart_backend.dart", "elements/elements.dart",
      "elements/modelx.dart", "js/js.dart", "js_backend/js_backend.dart", "js/precedence.dart",
      "mirrors/dart2js_mirror.dart", "mirrors/mirrors.dart", "mirrors/mirrors_util.dart",
      "mirrors/util.dart", "resolution/resolution.dart", "resolution/secret_tree_element.dart",
      "scanner/scannerlib.dart", "scanner/scanner_implementation.dart", "ssa/ssa.dart",
      "ssa/tracer.dart", "tree/tree.dart", "types/types.dart", "universe/universe.dart",
      "util/characters.dart", "util/uri_extras.dart", "util/util.dart",
      "util/util_implementation.dart"};

  public void test_dart2jsAnalysis() throws AnalysisException {
    String svnRootName = System.getProperty("svnRoot");
    assertNotNull("Missing property value: set using -DsvnRoot=...", svnRootName);
    File svnRoot = new File(svnRootName);
    assertTrue("Invalid property value: svnRoot directory does not exist", svnRoot.exists());

    DartSdk sdk = DirectoryBasedDartSdk.getDefaultSdk();
    assertNotNull(
        "Missing or invalid property value: set using -Dcom.google.dart.sdk=...",
        svnRootName);
    SourceFactory sourceFactory = new SourceFactory(new DartUriResolver(sdk), new FileUriResolver());
    AnalysisContext context = AnalysisEngine.getInstance().createAnalysisContext();
    context.setSourceFactory(sourceFactory);

    ArrayList<LibraryElement> libraries = new ArrayList<LibraryElement>();
    File dart2jsDir = new File(svnRoot, "sdk/lib/_internal/compiler/implementation/");
    long startTime = System.currentTimeMillis();
    for (String library : DART2JS_LIB_PATHS) {
      File libraryFile = new File(dart2jsDir, library);
      assertTrue("Invalid dart2js library file: " + libraryFile.toString(), libraryFile.exists());
      libraries.add(context.computeLibraryElement(new FileBasedSource(
          sourceFactory.getContentCache(),
          libraryFile)));
    }
    long endTime = System.currentTimeMillis();
    //
    // Print out timing information.
    //
    System.out.print("Resolved dart2js in ");
    System.out.print(endTime - startTime);
    System.out.println(" ms");
    System.out.println();
    //
    // Print out memory usage information.
    //
    LibraryElement[] libraryEltArray = libraries.toArray(new LibraryElement[libraries.size()]);
//    MemoryUsage usage = MemoryUtilities.measureMemoryUsage(libraryEltArray);
//    PrintWriter writer = new PrintWriter(System.out);
//    usage.writeSummary(writer);
//    writer.flush();
    //
    // Validate that there were no errors.
    //
    ArrayList<AnalysisError> errorList = new ArrayList<AnalysisError>();
    for (LibraryElement library : libraryEltArray) {
      addErrors(errorList, library.getDefiningCompilationUnit());
      for (CompilationUnitElement part : library.getParts()) {
        addErrors(errorList, part);
      }
    }
    assertErrors(errorList);
    //
    // Validate that the results were correctly formed.
    //
    StaticTypeVerifier staticTypeVerifier = new StaticTypeVerifier();
    ResolutionVerifier resolutionVerifier = new ResolutionVerifier();
    ElementStructureVerifier elementVerifier = new ElementStructureVerifier();
    for (LibraryElement library : libraryEltArray) {
      library.accept(elementVerifier);

      CompilationUnitElement definingCompilationUnitElement = library.getDefiningCompilationUnit();
      CompilationUnitElement[] compilationUnitElements = library.getParts();

      CompilationUnit definingCompilationUnit = context.resolveCompilationUnit(
          definingCompilationUnitElement.getSource(),
          library);
      definingCompilationUnit.accept(staticTypeVerifier);
      definingCompilationUnit.accept(resolutionVerifier);

      for (CompilationUnitElement compilationUnitElement : compilationUnitElements) {
        CompilationUnit compilationUnit = context.resolveCompilationUnit(
            compilationUnitElement.getSource(),
            library);
        compilationUnit.accept(staticTypeVerifier);
        compilationUnit.accept(resolutionVerifier);
      }
    }
    staticTypeVerifier.assertResolved();
    resolutionVerifier.assertResolved();
    elementVerifier.assertValid();
  }

  /**
   * Add the errors reported for the given compilation unit to the given list of errors.
   * 
   * @param errorList the list to which the errors are to be added
   * @param element the compilation unit whose errors are to be added
   * @throws AnalysisException if the errors could not be determined
   */
  protected void addErrors(ArrayList<AnalysisError> errorList, CompilationUnitElement element)
      throws AnalysisException {
    LibraryElement library = element.getLibrary();
    AnalysisContext context = library.getContext();
    CompilationUnit unit = context.resolveCompilationUnit(element.getSource(), library);
    AnalysisError[] errors = unit.getErrors();
    if (errors == null) {
      Assert.fail("The compilation unit \"" + element.getSource().getFullName()
          + "\" was not resolved");
    }
    for (AnalysisError error : errors) {
      errorList.add(error);
    }
  }

  /**
   * Assert that the errors in the error list match the expected behavior of the test.
   * 
   * @param errorExpected {@code true} if the test indicates that errors should be produced
   * @param expectedToFail {@code true} if the outcome is expected to be inverted from normal
   * @param errorList the list of errors that were produced for the files that were analyzed
   */
  protected void assertErrors(ArrayList<AnalysisError> errorList) {
    if (errorList.size() > 0) {
      PrintStringWriter writer = new PrintStringWriter();
      writer.print("Expected 0 errors, found ");
      writer.print(errorList.size());
      writer.print(":");
//      Collections.sort(errorList, AnalysisError.FILE_COMPARATOR);
      Collections.sort(errorList, AnalysisError.ERROR_CODE_COMPARATOR);
      for (AnalysisError error : errorList) {
        Source source = error.getSource();
        ErrorCode code = error.getErrorCode();
        int offset = error.getOffset();
        writer.println();
        writer.printf(
            "  %s %s (%d..%d) \"%s\"",
            source == null ? "null" : source.getShortName(),
            code.getClass().getSimpleName() + "." + code,
            offset,
            offset + error.getLength(),
            error.getMessage());
      }
      Assert.fail(writer.toString());
    }
  }
}
