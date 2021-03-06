/*
 * Copyright (c) 2012, the Dart project authors.
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
package editor;

import com.google.common.base.Joiner;
import com.google.common.util.concurrent.Uninterruptibles;
import com.google.dart.tools.core.test.util.TestProject;
import com.google.dart.tools.internal.corext.refactoring.util.ExecutionUtils;
import com.google.dart.tools.internal.corext.refactoring.util.RunnableEx;
import com.google.dart.tools.ui.internal.text.editor.DartEditor;
import com.google.dart.ui.test.HeadlessTestManager;
import com.google.dart.ui.test.model.Workbench;

import junit.framework.TestCase;

import org.eclipse.core.resources.IFile;
import org.eclipse.jface.action.IAction;
import org.eclipse.jface.text.source.ISourceViewer;
import org.eclipse.swt.custom.StyledText;
import org.eclipse.ui.IActionBars;
import org.eclipse.ui.IWorkbench;
import org.eclipse.ui.IWorkbenchPage;
import org.eclipse.ui.IWorkbenchWindow;
import org.eclipse.ui.PlatformUI;
import org.eclipse.ui.texteditor.AbstractTextEditor;

import static org.fest.assertions.Assertions.assertThat;

import java.lang.reflect.Method;
import java.util.concurrent.TimeUnit;

/**
 * Base for editor tab tests.
 */
public class AbstractDartEditorTabTest2 extends TestCase {
  protected final static String EOL = System.getProperty("line.separator", "\n");

  protected static TestProject testProject;

  protected static IWorkbenchPage activePage;

  protected static String makeSource(String... lines) {
    return Joiner.on(EOL).join(lines);
  }

  private static void waitForWorkbenchWindow() {
    while (activePage == null) {
      ExecutionUtils.runRethrowUI(new RunnableEx() {
        @Override
        public void run() throws Exception {
          IWorkbench workbench = PlatformUI.getWorkbench();
          if (workbench == null) {
            return;
          }
          IWorkbenchWindow activeWindow = workbench.getActiveWorkbenchWindow();
          if (activeWindow == null) {
            return;
          }
          activePage = activeWindow.getActivePage();
        }
      });
      Uninterruptibles.sleepUninterruptibly(5, TimeUnit.MILLISECONDS);
    }
  }

  private IFile testFile;
  protected DartEditor testEditor;
  protected ISourceViewer sourceViewer;

  protected StyledText textWidget;

  protected HeadlessTestManager headlessTestManager;

  /**
   * Asserts that the unit <code>test.dart</code> has the given content.
   */
  public void assertTestUnitContent(String... lines) throws Exception {
    assertEquals(makeSource(lines), getTestUnitContent());
  }

  /**
   * @return the content of the unit <code>test.dart</code>.
   */
  public String getTestUnitContent() throws Exception {
    return testProject.getFileString("test.dart");
  }

  /**
   * Opens {@link DartEditor} for unit <code>test.dart</code> with given content.
   */
  public void openTestEditor(String... lines) throws Exception {
    testFile = testProject.setFileContent("test.dart", makeSource(lines));
    testEditor = (DartEditor) Workbench.openEditor(testFile);
    // prepare ISourceViewer and StyledText
    {
      Method method = AbstractTextEditor.class.getDeclaredMethod("getSourceViewer");
      method.setAccessible(true);
      sourceViewer = (ISourceViewer) method.invoke(testEditor);
    }
    textWidget = sourceViewer.getTextWidget();
  }

  /**
   * @return the end position of the given pattern in the editor source. Fails in pattern was not
   *         found.
   */
  protected int findEnd(String pattern) {
    return findOffset(pattern) + pattern.length();
  }

  /**
   * @return the offset of the given pattern in the editor source. Fails in pattern was not found.
   */
  protected int findOffset(String pattern) {
    int position = sourceViewer.getDocument().get().indexOf(pattern);
    assertThat(position).isNotEqualTo(-1);
    return position;
  }

  /**
   * @return the {@link IAction} with given definition ID, may be <code>null</code>.
   */
  protected final IAction getEditorAction(String id) {
    IActionBars actionBars = testEditor.getEditorSite().getActionBars();
    {
      IAction action = testEditor.getAction(id);
      if (action != null) {
        return action;
      }
    }
    return actionBars.getGlobalActionHandler(id);
  }

  /**
   * Places caret into the position of the given pattern.
   */
  protected final void selectOffset(String pattern) throws Exception {
    final int position = findOffset(pattern);
    selectRange(position, position);
  }

  /**
   * Selects and reveals the range associated with the given pattern.
   */
  protected final void selectRange(final int start, final int end) throws Exception {
    ExecutionUtils.runRethrowUI(new RunnableEx() {
      @Override
      public void run() throws Exception {
        testEditor.selectAndReveal(start, end - start);
      }
    });
  }

  /**
   * Selects and reveals the range associated with the given pattern.
   */
  protected final void selectRange(String pattern) throws Exception {
    int position = findOffset(pattern);
    selectRange(position, position + pattern.length());
  }

  @Override
  protected void setUp() throws Exception {
    super.setUp();
    headlessTestManager = new HeadlessTestManager();
    headlessTestManager.install();
    waitForWorkbenchWindow();
    if (testProject == null) {
      testProject = new TestProject("sharedEditorTabProject");
    }
  }

  @Override
  protected void tearDown() throws Exception {
    Workbench.closeAllEditors();
    headlessTestManager.uninstall();
    super.tearDown();
  }
}
