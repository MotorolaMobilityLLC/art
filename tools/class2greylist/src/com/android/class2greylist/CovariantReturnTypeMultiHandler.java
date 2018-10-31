package com.android.class2greylist;

import com.google.common.annotations.VisibleForTesting;
import com.google.common.base.Preconditions;

import org.apache.bcel.classfile.AnnotationElementValue;
import org.apache.bcel.classfile.AnnotationEntry;
import org.apache.bcel.classfile.ArrayElementValue;
import org.apache.bcel.classfile.ElementValue;
import org.apache.bcel.classfile.ElementValuePair;

import java.util.Set;

/**
 * Handles {@code CovariantReturnType$CovariantReturnTypes} annotations, which
 * are generated by the compiler when multiple {@code CovariantReturnType}
 * annotations appear on a single method.
 *
 * <p>The enclosed annotations are passed to {@link CovariantReturnTypeHandler}.
 */
public class CovariantReturnTypeMultiHandler extends AnnotationHandler {

    public static final String ANNOTATION_NAME =
            "Ldalvik/annotation/codegen/CovariantReturnType$CovariantReturnTypes;";

    private static final String VALUE = "value";

    private final CovariantReturnTypeHandler mWrappedHandler;
    private final String mInnerAnnotationName;

    public CovariantReturnTypeMultiHandler(AnnotationConsumer consumer, Set<String> publicApis,
            String hiddenapiFlag) {
        this(consumer, publicApis, hiddenapiFlag, CovariantReturnTypeHandler.ANNOTATION_NAME);
    }

    @VisibleForTesting
    public CovariantReturnTypeMultiHandler(AnnotationConsumer consumer, Set<String> publicApis,
            String hiddenapiFlag, String innerAnnotationName) {
        mWrappedHandler = new CovariantReturnTypeHandler(consumer, publicApis, hiddenapiFlag);
        mInnerAnnotationName = innerAnnotationName;
    }

    @Override
    public void handleAnnotation(AnnotationEntry annotation, AnnotationContext context) {
        // Verify that the annotation has the form we expect
        ElementValuePair value = findValue(annotation);
        if (value == null) {
            context.reportError("No value found on CovariantReturnType$CovariantReturnTypes");
            return;
        }
        Preconditions.checkArgument(value.getValue() instanceof ArrayElementValue);
        ArrayElementValue array = (ArrayElementValue) value.getValue();

        // call wrapped handler on each enclosed annotation:
        for (ElementValue v : array.getElementValuesArray()) {
            Preconditions.checkArgument(v instanceof AnnotationElementValue);
            AnnotationElementValue aev = (AnnotationElementValue) v;
            Preconditions.checkArgument(
                    aev.getAnnotationEntry().getAnnotationType().equals(mInnerAnnotationName));
            mWrappedHandler.handleAnnotation(aev.getAnnotationEntry(), context);
        }
    }

    private ElementValuePair findValue(AnnotationEntry a) {
        for (ElementValuePair property : a.getElementValuePairs()) {
            if (property.getNameString().equals(VALUE)) {
                return property;
            }
        }
        // not found
        return null;
    }
}
