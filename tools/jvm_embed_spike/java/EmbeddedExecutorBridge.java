// Bridge between the JNI host and the REAL contract-executor.jar's
// ContractExecutorHandler. Two modes:
//   - "lite" handler: built with null ceService. Sufficient for
//     getExecutorBuildVersion which doesn't touch ceService.
//   - "full" handler: built via the production Dagger graph (same as
//     ExecutorApp.main) so methods that DO touch ceService work
//     (compileSourceCode etc.). The Dagger builder's provider field is
//     extracted via reflection — a brittle pattern acceptable for the
//     spike; production would expose a static accessor.

import com.credits.ApplicationProperties;
import com.credits.DaggerExecutorApp_ContractExecutorServerBuilder;
import com.credits.thrift.ContractExecutorHandler;
import org.apache.thrift.TSerializer;
import org.apache.thrift.TDeserializer;
import org.apache.thrift.protocol.TBinaryProtocol;

import com.credits.client.executor.thrift.generated.ExecutorBuildVersionResult;
import com.credits.client.executor.thrift.generated.CompileSourceCodeResult;
import com.credits.client.executor.thrift.generated.GetContractMethodsResult;
import com.credits.client.executor.thrift.generated.GetContractVariablesResult;
import com.credits.client.executor.thrift.generated.ExecuteByteCodeResult;
import com.credits.client.executor.thrift.generated.SetterMethodResult;
import com.credits.client.executor.thrift.generated.SmartContractBinary;
import com.credits.client.executor.thrift.generated.MethodHeader;
import com.credits.general.thrift.generated.ClassObject;
import com.credits.general.thrift.generated.ByteCodeObject;
import com.credits.general.thrift.generated.Annotation;
import com.credits.general.thrift.generated.MethodArgument;
import com.credits.general.thrift.generated.MethodDescription;
import com.credits.general.thrift.generated.Variant;

import java.lang.reflect.Field;
import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

public class EmbeddedExecutorBridge {

    // Mirrors StubExecutor.BuildVersionResult so the C++ side reuses the
    // same field-extraction code (callGetExecutorBuildVersion handler).
    public static final class BuildVersionResult {
        public final byte   code;
        public final String message;
        public final int    commitNumber;
        public final String commitHash;

        public BuildVersionResult(byte code, String message, int commitNumber, String commitHash) {
            this.code = code;
            this.message = message;
            this.commitNumber = commitNumber;
            this.commitHash = commitHash;
        }
    }

    // Lazy-initialised, cached. First access triggers full Dagger bootstrap.
    private static volatile ContractExecutorHandler fullHandler;

    private static synchronized ContractExecutorHandler getFullHandler() throws Exception {
        if (fullHandler != null) return fullHandler;
        // Build the same Dagger graph that ExecutorApp.main builds.
        Object builder = DaggerExecutorApp_ContractExecutorServerBuilder.builder().build();
        // Field name matches the Dagger-generated provider list we observed via
        // javap. If a future executor build renames it, this throws, the bridge
        // catches, and the caller sees a non-zero code.
        Field f = builder.getClass().getDeclaredField("providesContractExecutorHandlerProvider");
        f.setAccessible(true);
        @SuppressWarnings("unchecked")
        javax.inject.Provider<ContractExecutorHandler> p =
            (javax.inject.Provider<ContractExecutorHandler>) f.get(builder);
        fullHandler = p.get();
        return fullHandler;
    }

    public static BuildVersionResult getExecutorBuildVersion(short version) {
        try {
            // Lightweight path — null ceService is acceptable here because the
            // implementation only touches ApplicationProperties.
            ApplicationProperties props = new ApplicationProperties();
            ContractExecutorHandler handler = new ContractExecutorHandler(null, props);
            ExecutorBuildVersionResult r = handler.getExecutorBuildVersion(version);

            byte code = (r.status != null) ? r.status.code : (byte) -1;
            String message = (r.status != null && r.status.message != null) ? r.status.message : "";
            String hash = (r.commitHash != null) ? r.commitHash : "";
            return new BuildVersionResult(code, message, r.commitNumber, hash);
        } catch (Throwable t) {
            return new BuildVersionResult((byte) 1, "bridge: " + t.toString(), 0, "");
        }
    }

    // ---- compileSourceCode -------------------------------------------------
    // Mirrors CompileSourceCodeResult { APIResponse status; List<ByteCodeObject> }
    // ByteCodeObject { String name; ByteBuffer byteCode } — flattened to
    // parallel arrays here since C++ has no notion of List<Object>.

    public static final class CompiledClass {
        public final String name;
        public final byte[] byteCode;
        public CompiledClass(String name, byte[] byteCode) {
            this.name = name; this.byteCode = byteCode;
        }
    }

    public static final class CompileResult {
        public final byte   code;
        public final String message;
        public final CompiledClass[] classes;
        public CompileResult(byte code, String message, CompiledClass[] classes) {
            this.code = code; this.message = message; this.classes = classes;
        }
    }

    // ---- getContractMethods ------------------------------------------------
    // Demonstrates the OPPOSITE direction of List<Struct> marshalling: C++
    // builds parallel arrays (class names + bytecodes), the bridge constructs
    // List<ByteCodeObject>, calls the handler, then flattens the resulting
    // List<MethodDescription> back into parallel arrays for C++ to read.

    public static final class AnnotationInfo {
        public final String   name;
        // Annotation arguments are Map<String,String> in Thrift; flatten to
        // parallel arrays so the C++ side can read them without a Map iterator.
        public final String[] argNames;
        public final String[] argValues;
        public AnnotationInfo(String name, String[] argNames, String[] argValues) {
            this.name = name; this.argNames = argNames; this.argValues = argValues;
        }
    }

    public static final class MethodArgumentInfo {
        public final String           type;
        public final String           name;
        public final AnnotationInfo[] annotations;
        public MethodArgumentInfo(String type, String name, AnnotationInfo[] annotations) {
            this.type = type; this.name = name; this.annotations = annotations;
        }
    }

    public static final class MethodInfo {
        public final String                name;
        public final String                returnType;
        public final MethodArgumentInfo[]  arguments;
        public final AnnotationInfo[]      annotations;
        public MethodInfo(String name, String returnType,
                          MethodArgumentInfo[] arguments, AnnotationInfo[] annotations) {
            this.name = name; this.returnType = returnType;
            this.arguments = arguments; this.annotations = annotations;
        }
    }

    public static final class MethodsResult {
        public final byte         code;
        public final String       message;
        public final MethodInfo[] methods;
        public MethodsResult(byte code, String message, MethodInfo[] methods) {
            this.code = code; this.message = message; this.methods = methods;
        }
    }

    private static AnnotationInfo[] flattenAnnotations(java.util.List<Annotation> anns) {
        if (anns == null || anns.isEmpty()) return new AnnotationInfo[0];
        AnnotationInfo[] out = new AnnotationInfo[anns.size()];
        for (int i = 0; i < anns.size(); ++i) {
            Annotation a = anns.get(i);
            String name = a.name != null ? a.name : "";
            java.util.Map<String, String> args = a.arguments;
            String[] keys = (args != null && !args.isEmpty()) ? new String[args.size()] : new String[0];
            String[] vals = (args != null && !args.isEmpty()) ? new String[args.size()] : new String[0];
            if (args != null && !args.isEmpty()) {
                int j = 0;
                for (Map.Entry<String, String> e : args.entrySet()) {
                    keys[j] = e.getKey() != null ? e.getKey() : "";
                    vals[j] = e.getValue() != null ? e.getValue() : "";
                    ++j;
                }
            }
            out[i] = new AnnotationInfo(name, keys, vals);
        }
        return out;
    }

    private static MethodArgumentInfo[] flattenArgs(java.util.List<MethodArgument> args) {
        if (args == null || args.isEmpty()) return new MethodArgumentInfo[0];
        MethodArgumentInfo[] out = new MethodArgumentInfo[args.size()];
        for (int i = 0; i < args.size(); ++i) {
            MethodArgument ma = args.get(i);
            out[i] = new MethodArgumentInfo(
                ma.type != null ? ma.type : "",
                ma.name != null ? ma.name : "",
                flattenAnnotations(ma.annotations));
        }
        return out;
    }

    // ---- Variant unwrap helpers --------------------------------------------
    // Variant is a 25-case Thrift union. Rather than expose the union shape
    // to C++, we flatten each Variant into a tagged C++-friendly struct.
    // Common cases get first-class fields; everything else falls back to
    // the toString() representation. This is the right tradeoff for the
    // spike: real production users rarely need anything beyond primitives,
    // and the long tail (V_OBJECT, V_AMOUNT, V_LIST/MAP of Variants) needs
    // recursive design that's better done with proper review.

    public static final int VTAG_NULL    = 0;
    public static final int VTAG_BOOL    = 1;
    public static final int VTAG_LONG    = 2;   // covers byte/short/int/long (boxed and not)
    public static final int VTAG_DOUBLE  = 3;   // covers float/double (boxed and not)
    public static final int VTAG_STRING  = 4;   // covers v_string and v_big_decimal
    public static final int VTAG_BYTES   = 5;
    public static final int VTAG_OTHER   = 6;   // V_OBJECT, V_LIST, V_MAP, V_SET, V_ARRAY
                                                // — repr only, no structured access from C++.
    public static final int VTAG_VOID    = 7;   // V_VOID — placeholder return for void-returning methods
    public static final int VTAG_AMOUNT  = 8;   // V_AMOUNT (currency: int integral + long fraction).
                                                // Encoded as longVal=integral, bytesVal=8-byte BE fraction.
    public static final int VTAG_THRIFT_BINARY = 9; // Thrift-binary-serialized Variant tree.
                                                    // bytesVal contains the serialized blob; both
                                                    // sides round-trip via TBinaryProtocol so any
                                                    // Variant case (including V_LIST/MAP/OBJECT)
                                                    // survives across the JNI boundary.

    public static final class TaggedVariant {
        public final int     tag;
        public final boolean boolVal;
        public final long    longVal;
        public final double  doubleVal;
        public final String  stringVal;
        public final byte[]  bytesVal;
        public final String  repr;        // toString() for OTHER and as a fallback debug aid

        TaggedVariant(int tag, boolean b, long l, double d, String s, byte[] by, String repr) {
            this.tag = tag; this.boolVal = b; this.longVal = l; this.doubleVal = d;
            this.stringVal = s; this.bytesVal = by; this.repr = repr;
        }
    }

    private static TaggedVariant flattenVariant(Variant v) {
        if (v == null) {
            return new TaggedVariant(VTAG_NULL, false, 0, 0, null, null, "null");
        }
        try {
            if (v.isSetV_boolean())     return new TaggedVariant(VTAG_BOOL,   v.getV_boolean(),  0, 0, null, null, null);
            if (v.isSetV_boolean_box()) return new TaggedVariant(VTAG_BOOL,   v.getV_boolean_box(),0,0, null, null, null);
            if (v.isSetV_byte())        return new TaggedVariant(VTAG_LONG,   false, v.getV_byte(),   0, null, null, null);
            if (v.isSetV_byte_box())    return new TaggedVariant(VTAG_LONG,   false, v.getV_byte_box(),0, null, null, null);
            if (v.isSetV_short())       return new TaggedVariant(VTAG_LONG,   false, v.getV_short(),  0, null, null, null);
            if (v.isSetV_short_box())   return new TaggedVariant(VTAG_LONG,   false, v.getV_short_box(),0,null, null, null);
            if (v.isSetV_int())         return new TaggedVariant(VTAG_LONG,   false, v.getV_int(),    0, null, null, null);
            if (v.isSetV_int_box())     return new TaggedVariant(VTAG_LONG,   false, v.getV_int_box(),0, null, null, null);
            if (v.isSetV_long())        return new TaggedVariant(VTAG_LONG,   false, v.getV_long(),   0, null, null, null);
            if (v.isSetV_long_box())    return new TaggedVariant(VTAG_LONG,   false, v.getV_long_box(),0,null, null, null);
            if (v.isSetV_float())       return new TaggedVariant(VTAG_DOUBLE, false, 0, v.getV_float(),  null, null, null);
            if (v.isSetV_float_box())   return new TaggedVariant(VTAG_DOUBLE, false, 0, v.getV_float_box(),null,null, null);
            if (v.isSetV_double())      return new TaggedVariant(VTAG_DOUBLE, false, 0, v.getV_double(), null, null, null);
            if (v.isSetV_double_box())  return new TaggedVariant(VTAG_DOUBLE, false, 0, v.getV_double_box(),null,null,null);
            if (v.isSetV_string())      return new TaggedVariant(VTAG_STRING, false, 0, 0, v.getV_string(),     null, null);
            if (v.isSetV_big_decimal()) return new TaggedVariant(VTAG_STRING, false, 0, 0, v.getV_big_decimal(), null, null);
            if (v.isSetV_null())        return new TaggedVariant(VTAG_NULL,   false, 0, 0, null, null, "null");
            if (v.isSetV_void())        return new TaggedVariant(VTAG_VOID,   false, 0, 0, null, null, null);
            if (v.isSetV_byte_array())  return new TaggedVariant(VTAG_BYTES,  false, 0, 0, null, v.getV_byte_array(), null);
            if (v.isSetV_amount()) {
                com.credits.general.thrift.generated.Amount a = v.getV_amount();
                long integral = (a != null) ? a.integral : 0;
                long fraction = (a != null) ? a.fraction : 0;
                byte[] frac8 = new byte[8];
                for (int i = 7; i >= 0; --i) { frac8[i] = (byte) (fraction & 0xFF); fraction >>>= 8; }
                return new TaggedVariant(VTAG_AMOUNT, false, integral, 0, null, frac8, null);
            }
            // Recursive / structured cases (V_LIST / V_MAP / V_SET / V_ARRAY /
            // V_OBJECT). Serialize the entire Variant via Thrift binary protocol;
            // C++ side deserializes back into a general::Variant. Lossless.
            try {
                TSerializer ser = new TSerializer(new TBinaryProtocol.Factory());
                byte[] blob = ser.serialize(v);
                return new TaggedVariant(VTAG_THRIFT_BINARY, false, 0, 0, null, blob, null);
            } catch (Throwable serErr) {
                // If serialization itself fails (truly degenerate Variant), fall
                // through to the toString debug repr.
                return new TaggedVariant(VTAG_OTHER, false, 0, 0, null, null, v.toString());
            }
        } catch (Throwable t) {
            return new TaggedVariant(VTAG_OTHER, false, 0, 0, null, null, "variant-error: " + t);
        }
    }

    // ---- getContractVariables ----------------------------------------------
    // Result is Map<String, Variant>. We flatten to two parallel arrays.

    public static final class VariablesResult {
        public final byte             code;
        public final String           message;
        public final String[]         names;
        public final TaggedVariant[]  values;
        public VariablesResult(byte code, String message, String[] names, TaggedVariant[] values) {
            this.code = code; this.message = message; this.names = names; this.values = values;
        }
    }

    public static VariablesResult getContractVariables(
            String[] classNames, byte[][] byteCodes, byte[] state, short version) {
        try {
            if (classNames == null || byteCodes == null || classNames.length != byteCodes.length) {
                return new VariablesResult((byte) 1, "bridge: bad input arrays", new String[0], new TaggedVariant[0]);
            }
            ContractExecutorHandler handler = getFullHandler();

            List<ByteCodeObject> in = new ArrayList<>(classNames.length);
            for (int i = 0; i < classNames.length; ++i) {
                ByteCodeObject obj = new ByteCodeObject();
                obj.name = classNames[i];
                obj.byteCode = ByteBuffer.wrap(byteCodes[i] != null ? byteCodes[i] : new byte[0]);
                in.add(obj);
            }
            ByteBuffer stateBuf = ByteBuffer.wrap(state != null ? state : new byte[0]);

            GetContractVariablesResult r = handler.getContractVariables(in, stateBuf, version);

            byte code = (r.status != null) ? r.status.code : (byte) -1;
            String message = (r.status != null && r.status.message != null) ? r.status.message : "";

            String[] names;
            TaggedVariant[] values;
            if (r.contractVariables != null) {
                int n = r.contractVariables.size();
                names = new String[n];
                values = new TaggedVariant[n];
                int i = 0;
                for (Map.Entry<String, Variant> e : r.contractVariables.entrySet()) {
                    names[i]  = e.getKey() != null ? e.getKey() : "";
                    values[i] = flattenVariant(e.getValue());
                    ++i;
                }
            } else {
                names  = new String[0];
                values = new TaggedVariant[0];
            }
            return new VariablesResult(code, message, names, values);
        } catch (Throwable t) {
            return new VariablesResult((byte) 1, "bridge: " + t.toString(), new String[0], new TaggedVariant[0]);
        }
    }

    // ---- executeByteCode ---------------------------------------------------
    // Calls the real handler.executeByteCode. Inputs are flattened parallel
    // arrays; output exposes the first SetterMethodResult's return-Variant
    // and execution cost. Multi-method results (executeByteCodeMultiple)
    // would extend this with per-method arrays.

    public static final class ExecuteResult {
        public final byte          code;          // overall status
        public final String        message;
        public final byte          methodCode;    // first SetterMethodResult.status.code
        public final String        methodMessage;
        public final TaggedVariant retVal;        // first SetterMethodResult.ret_val
        public final long          executionCost;
        public final byte[]        newState;      // contractsState[<contract addr>], if produced
        public ExecuteResult(byte code, String message,
                             byte methodCode, String methodMessage,
                             TaggedVariant retVal, long executionCost, byte[] newState) {
            this.code = code; this.message = message;
            this.methodCode = methodCode; this.methodMessage = methodMessage;
            this.retVal = retVal; this.executionCost = executionCost;
            this.newState = newState;
        }
    }

    public static ExecuteResult executeByteCode(
            long accessId,
            byte[] initiatorAddress,
            byte[] contractAddress,
            String[] classNames, byte[][] byteCodes,
            byte[] instance, boolean stateCanModify,
            String methodName,
            long executionTime,
            short version) {
        try {
            if (classNames == null || byteCodes == null || classNames.length != byteCodes.length) {
                return new ExecuteResult((byte) 1, "bridge: bad input arrays",
                                         (byte) 1, "", flattenVariant(null), 0, new byte[0]);
            }
            ContractExecutorHandler handler = getFullHandler();

            List<ByteCodeObject> bcs = new ArrayList<>(classNames.length);
            for (int i = 0; i < classNames.length; ++i) {
                ByteCodeObject obj = new ByteCodeObject();
                obj.name = classNames[i];
                obj.byteCode = ByteBuffer.wrap(byteCodes[i] != null ? byteCodes[i] : new byte[0]);
                bcs.add(obj);
            }
            ClassObject classObject = new ClassObject();
            classObject.byteCodeObjects = bcs;
            classObject.instance = ByteBuffer.wrap(instance != null ? instance : new byte[0]);

            SmartContractBinary scb = new SmartContractBinary();
            scb.contractAddress = ByteBuffer.wrap(
                contractAddress != null ? contractAddress : new byte[32]);
            scb.object = classObject;
            scb.setStateCanModify(stateCanModify);

            List<MethodHeader> headers = new ArrayList<>();
            MethodHeader header = new MethodHeader();
            header.methodName = methodName != null ? methodName : "";
            header.params = new ArrayList<>();   // no params for the spike
            headers.add(header);

            ByteBuffer initiator = ByteBuffer.wrap(
                initiatorAddress != null ? initiatorAddress : new byte[32]);

            ExecuteByteCodeResult r = handler.executeByteCode(
                accessId, initiator, scb, headers, executionTime, version);

            byte code = (r.status != null) ? r.status.code : (byte) -1;
            String message = (r.status != null && r.status.message != null) ? r.status.message : "";

            byte mCode = -1;
            String mMessage = "";
            TaggedVariant ret = flattenVariant(null);
            long cost = 0;
            byte[] newState = new byte[0];

            if (r.results != null && !r.results.isEmpty()) {
                SetterMethodResult sr = r.results.get(0);
                if (sr.status != null) {
                    mCode = sr.status.code;
                    mMessage = sr.status.message != null ? sr.status.message : "";
                }
                ret = flattenVariant(sr.ret_val);
                cost = sr.executionCost;
                if (sr.contractsState != null && !sr.contractsState.isEmpty()) {
                    // Take the first state entry — for a single-contract call
                    // that's the only one and it's the new state of `contractAddress`.
                    Map.Entry<ByteBuffer, ByteBuffer> first = sr.contractsState.entrySet().iterator().next();
                    if (first.getValue() != null) {
                        ByteBuffer bb = first.getValue().duplicate();
                        newState = new byte[bb.remaining()];
                        bb.get(newState);
                    }
                }
            }
            return new ExecuteResult(code, message, mCode, mMessage, ret, cost, newState);
        } catch (Throwable t) {
            return new ExecuteResult(
                (byte) 1, "bridge: " + t.toString(),
                (byte) 1, "", flattenVariant(null), 0, new byte[0]);
        }
    }

    // ---- Variant marshalling self-test -------------------------------------
    // Constructs every Thrift Variant case, flattens it through flattenVariant,
    // and reports the resulting TaggedVariant alongside the tag we expect.
    // Run once at startup so a missing case in flattenVariant or a stale
    // bounds check in the C++ tag-clamp shows up as a startup error rather
    // than as a flood of fallback warnings on the live chain.

    public static final class CaseReport {
        public final String        name;
        public final int           expectedTag;
        public final TaggedVariant flattened;
        public CaseReport(String n, int e, TaggedVariant t) {
            this.name = n; this.expectedTag = e; this.flattened = t;
        }
    }

    public static CaseReport[] runVariantSelfTest() {
        java.util.ArrayList<CaseReport> r = new java.util.ArrayList<>();
        Variant v;

        // Primitive scalar cases.
        v = new Variant(); v.setV_null("");                       r.add(new CaseReport("v_null",         VTAG_NULL,   flattenVariant(v)));
        v = new Variant(); v.setV_void((byte) 0);                 r.add(new CaseReport("v_void",         VTAG_VOID,   flattenVariant(v)));
        v = new Variant(); v.setV_boolean(true);                  r.add(new CaseReport("v_boolean",      VTAG_BOOL,   flattenVariant(v)));
        v = new Variant(); v.setV_boolean_box(true);              r.add(new CaseReport("v_boolean_box",  VTAG_BOOL,   flattenVariant(v)));
        v = new Variant(); v.setV_byte((byte) 42);                r.add(new CaseReport("v_byte",         VTAG_LONG,   flattenVariant(v)));
        v = new Variant(); v.setV_byte_box((byte) 42);            r.add(new CaseReport("v_byte_box",     VTAG_LONG,   flattenVariant(v)));
        v = new Variant(); v.setV_short((short) 1000);            r.add(new CaseReport("v_short",        VTAG_LONG,   flattenVariant(v)));
        v = new Variant(); v.setV_short_box((short) 1000);        r.add(new CaseReport("v_short_box",    VTAG_LONG,   flattenVariant(v)));
        v = new Variant(); v.setV_int(123456);                    r.add(new CaseReport("v_int",          VTAG_LONG,   flattenVariant(v)));
        v = new Variant(); v.setV_int_box(123456);                r.add(new CaseReport("v_int_box",      VTAG_LONG,   flattenVariant(v)));
        v = new Variant(); v.setV_long(9999999999L);              r.add(new CaseReport("v_long",         VTAG_LONG,   flattenVariant(v)));
        v = new Variant(); v.setV_long_box(9999999999L);          r.add(new CaseReport("v_long_box",     VTAG_LONG,   flattenVariant(v)));
        v = new Variant(); v.setV_float(3.14);                    r.add(new CaseReport("v_float",        VTAG_DOUBLE, flattenVariant(v)));
        v = new Variant(); v.setV_float_box(3.14);                r.add(new CaseReport("v_float_box",    VTAG_DOUBLE, flattenVariant(v)));
        v = new Variant(); v.setV_double(2.71828);                r.add(new CaseReport("v_double",       VTAG_DOUBLE, flattenVariant(v)));
        v = new Variant(); v.setV_double_box(2.71828);            r.add(new CaseReport("v_double_box",   VTAG_DOUBLE, flattenVariant(v)));
        v = new Variant(); v.setV_string("hello");                r.add(new CaseReport("v_string",       VTAG_STRING, flattenVariant(v)));
        v = new Variant(); v.setV_big_decimal("123.456");         r.add(new CaseReport("v_big_decimal",  VTAG_STRING, flattenVariant(v)));
        v = new Variant(); v.setV_byte_array(new byte[]{1,2,3});  r.add(new CaseReport("v_byte_array",   VTAG_BYTES,  flattenVariant(v)));

        com.credits.general.thrift.generated.Amount amt = new com.credits.general.thrift.generated.Amount();
        amt.integral = 5; amt.fraction = 1234567890L;
        v = new Variant(); v.setV_amount(amt);                    r.add(new CaseReport("v_amount", VTAG_AMOUNT, flattenVariant(v)));

        // Recursive / structured cases — Phase 4J transports them as Thrift-
        // binary blobs and reconstructs on the C++ side, so they should now
        // come back as VTAG_THRIFT_BINARY (lossless round-trip).
        v = new Variant(); v.setV_list (new java.util.ArrayList<Variant>());                        r.add(new CaseReport("v_list",  VTAG_THRIFT_BINARY, flattenVariant(v)));
        v = new Variant(); v.setV_set  (new java.util.HashSet<Variant>());                          r.add(new CaseReport("v_set",   VTAG_THRIFT_BINARY, flattenVariant(v)));
        v = new Variant(); v.setV_map  (new java.util.HashMap<Variant, Variant>());                 r.add(new CaseReport("v_map",   VTAG_THRIFT_BINARY, flattenVariant(v)));
        v = new Variant(); v.setV_array(new java.util.ArrayList<Variant>());                        r.add(new CaseReport("v_array", VTAG_THRIFT_BINARY, flattenVariant(v)));

        // Round-trip the input direction too: each tag we know how to BUILD,
        // when then flattened, must come back as the same tag.
        int[] inputTags = { VTAG_NULL, VTAG_VOID, VTAG_BOOL, VTAG_LONG, VTAG_DOUBLE,
                            VTAG_STRING, VTAG_BYTES };
        for (int tag : inputTags) {
            Variant built = buildVariantFromTagged(tag, true, 42L, 3.14,
                                                    "test", new byte[]{1,2,3});
            r.add(new CaseReport("input->flatten:tag=" + tag, tag, flattenVariant(built)));
        }
        // V_AMOUNT input round-trip needs an 8-byte fraction blob.
        byte[] fracBlob = new byte[8];
        long fracIn = 1234567890L;
        for (int i = 7; i >= 0; --i) { fracBlob[i] = (byte)(fracIn & 0xFF); fracIn >>>= 8; }
        Variant amtBuilt = buildVariantFromTagged(VTAG_AMOUNT, false, 5L, 0.0, null, fracBlob);
        r.add(new CaseReport("input->flatten:tag=" + VTAG_AMOUNT, VTAG_AMOUNT, flattenVariant(amtBuilt)));

        return r.toArray(new CaseReport[0]);
    }

    // ---- Full executeByteCode (production-fidelity) -----------------------
    // Returns the COMPLETE Thrift result shape so cs::Executor can populate
    // its Thrift _return without any data loss. Critical for slow-start
    // replay correctness.

    public static final class FullStateEntry {
        public final byte[] address;     // 32-byte contract address
        public final byte[] state;       // serialized contract state
        public FullStateEntry(byte[] a, byte[] s) { address = a; state = s; }
    }

    public static final class FullEmittedTxn {
        public final byte[] source;
        public final byte[] target;
        public final int    amountIntegral;
        public final long   amountFraction;
        public final byte[] userData;
        public FullEmittedTxn(byte[] src, byte[] tgt, int integral, long fraction, byte[] data) {
            this.source = src; this.target = tgt;
            this.amountIntegral = integral; this.amountFraction = fraction;
            this.userData = data;
        }
    }

    public static final class FullSetterResult {
        public final byte             code;
        public final String           message;
        public final TaggedVariant    retVal;
        public final FullStateEntry[] contractsState;
        public final FullEmittedTxn[] emittedTransactions;
        public final long             executionCost;
        public FullSetterResult(byte code, String msg, TaggedVariant ret,
                                FullStateEntry[] state, FullEmittedTxn[] emitted, long cost) {
            this.code = code; this.message = msg; this.retVal = ret;
            this.contractsState = state; this.emittedTransactions = emitted;
            this.executionCost = cost;
        }
    }

    public static final class FullExecuteResult {
        public final byte                code;
        public final String              message;
        public final FullSetterResult[]  results;
        public FullExecuteResult(byte code, String msg, FullSetterResult[] results) {
            this.code = code; this.message = msg; this.results = results;
        }
    }

    // Build a single Java Variant from the parallel-array input form. C++
    // side passes one slot's data via these arguments; we honor the tag and
    // pull the corresponding slot.
    private static Variant buildVariantFromTagged(int tag,
                                                  boolean boolVal,
                                                  long longVal,
                                                  double doubleVal,
                                                  String stringVal,
                                                  byte[] bytesVal) {
        Variant v = new Variant();
        switch (tag) {
            case VTAG_BOOL:   v.setV_boolean(boolVal); break;
            case VTAG_LONG:   v.setV_long(longVal); break;
            case VTAG_DOUBLE: v.setV_double(doubleVal); break;
            case VTAG_STRING: v.setV_string(stringVal != null ? stringVal : ""); break;
            case VTAG_BYTES:  v.setV_byte_array(bytesVal != null ? bytesVal : new byte[0]); break;
            case VTAG_VOID:   v.setV_void((byte) 0); break;
            case VTAG_AMOUNT: {
                long fraction = 0;
                if (bytesVal != null && bytesVal.length == 8) {
                    for (int i = 0; i < 8; ++i) fraction = (fraction << 8) | (bytesVal[i] & 0xFFL);
                }
                com.credits.general.thrift.generated.Amount a = new com.credits.general.thrift.generated.Amount();
                a.integral = (int) longVal;
                a.fraction = fraction;
                v.setV_amount(a);
                break;
            }
            case VTAG_THRIFT_BINARY: {
                if (bytesVal != null && bytesVal.length > 0) {
                    try {
                        TDeserializer des = new TDeserializer(new TBinaryProtocol.Factory());
                        des.deserialize(v, bytesVal);
                    } catch (Throwable t) {
                        // If deserialization fails, leave v as default-constructed
                        // (all fields unset) — caller sees an empty Variant rather
                        // than a corrupted one.
                    }
                }
                break;
            }
            case VTAG_NULL:
            default:          v.setV_null(""); break;
        }
        return v;
    }

    public static FullExecuteResult executeByteCodeFull(
            long accessId,
            byte[] initiatorAddress,
            byte[] contractAddress,
            String[] classNames, byte[][] byteCodes,
            byte[] instance, boolean stateCanModify,
            // One entry per MethodHeader. methodNames.length must equal
            // paramGroupSizes.length. Sum(paramGroupSizes) == flat-param length.
            String[] methodNames,
            int[]    paramGroupSizes,
            int[]     paramTags,
            boolean[] paramBools,
            long[]    paramLongs,
            double[]  paramDoubles,
            String[]  paramStrings,
            byte[][]  paramBytes,
            long executionTime,
            short version) {
        try {
            ContractExecutorHandler handler = getFullHandler();

            // Build SmartContractBinary.
            List<ByteCodeObject> bcs = new ArrayList<>(classNames.length);
            for (int i = 0; i < classNames.length; ++i) {
                ByteCodeObject obj = new ByteCodeObject();
                obj.name = classNames[i];
                obj.byteCode = ByteBuffer.wrap(byteCodes[i] != null ? byteCodes[i] : new byte[0]);
                bcs.add(obj);
            }
            ClassObject classObject = new ClassObject();
            classObject.byteCodeObjects = bcs;
            classObject.instance = ByteBuffer.wrap(instance != null ? instance : new byte[0]);

            SmartContractBinary scb = new SmartContractBinary();
            scb.contractAddress = ByteBuffer.wrap(
                contractAddress != null ? contractAddress : new byte[32]);
            scb.object = classObject;
            scb.setStateCanModify(stateCanModify);

            // Build N MethodHeaders, each with its own param sublist sliced
            // out of the flat arrays via paramGroupSizes.
            List<MethodHeader> headers = new ArrayList<>(methodNames.length);
            int cursor = 0;
            for (int hi = 0; hi < methodNames.length; ++hi) {
                int sz = paramGroupSizes[hi];
                List<Variant> subParams = new ArrayList<>(sz);
                for (int j = 0; j < sz; ++j) {
                    int idx = cursor + j;
                    subParams.add(buildVariantFromTagged(
                        paramTags[idx],
                        paramBools   != null && idx < paramBools.length   ? paramBools[idx]   : false,
                        paramLongs   != null && idx < paramLongs.length   ? paramLongs[idx]   : 0L,
                        paramDoubles != null && idx < paramDoubles.length ? paramDoubles[idx] : 0.0,
                        paramStrings != null && idx < paramStrings.length ? paramStrings[idx] : null,
                        paramBytes   != null && idx < paramBytes.length   ? paramBytes[idx]   : null));
                }
                cursor += sz;
                MethodHeader header = new MethodHeader();
                header.methodName = methodNames[hi] != null ? methodNames[hi] : "";
                header.params = subParams;
                headers.add(header);
            }

            ByteBuffer initiator = ByteBuffer.wrap(
                initiatorAddress != null ? initiatorAddress : new byte[32]);

            ExecuteByteCodeResult r = handler.executeByteCode(
                accessId, initiator, scb, headers, executionTime, version);

            byte code    = (r.status != null) ? r.status.code : (byte) -1;
            String message = (r.status != null && r.status.message != null) ? r.status.message : "";

            FullSetterResult[] outResults;
            if (r.results != null) {
                outResults = new FullSetterResult[r.results.size()];
                for (int i = 0; i < r.results.size(); ++i) {
                    SetterMethodResult sr = r.results.get(i);

                    byte sCode = (sr.status != null) ? sr.status.code : (byte) 0;
                    String sMsg = (sr.status != null && sr.status.message != null) ? sr.status.message : "";

                    TaggedVariant ret = flattenVariant(sr.ret_val);

                    FullStateEntry[] stateArr;
                    if (sr.contractsState != null && !sr.contractsState.isEmpty()) {
                        stateArr = new FullStateEntry[sr.contractsState.size()];
                        int j = 0;
                        for (Map.Entry<ByteBuffer, ByteBuffer> e : sr.contractsState.entrySet()) {
                            byte[] addr  = bbToBytes(e.getKey());
                            byte[] sBuf  = bbToBytes(e.getValue());
                            stateArr[j++] = new FullStateEntry(addr, sBuf);
                        }
                    } else {
                        stateArr = new FullStateEntry[0];
                    }

                    FullEmittedTxn[] emitted;
                    if (sr.emittedTransactions != null && !sr.emittedTransactions.isEmpty()) {
                        emitted = new FullEmittedTxn[sr.emittedTransactions.size()];
                        for (int j = 0; j < sr.emittedTransactions.size(); ++j) {
                            com.credits.client.executor.thrift.generated.EmittedTransaction et = sr.emittedTransactions.get(j);
                            byte[] src = bbToBytes(et.source);
                            byte[] tgt = bbToBytes(et.target);
                            int  ai = (et.amount != null) ? et.amount.integral : 0;
                            long af = (et.amount != null) ? et.amount.fraction : 0L;
                            byte[] ud = bbToBytes(et.userData);
                            emitted[j] = new FullEmittedTxn(src, tgt, ai, af, ud);
                        }
                    } else {
                        emitted = new FullEmittedTxn[0];
                    }

                    outResults[i] = new FullSetterResult(sCode, sMsg, ret, stateArr, emitted, sr.executionCost);
                }
            } else {
                outResults = new FullSetterResult[0];
            }

            return new FullExecuteResult(code, message, outResults);
        } catch (Throwable t) {
            return new FullExecuteResult((byte) 1, "bridge: " + t.toString(), new FullSetterResult[0]);
        }
    }

    // executeByteCodeMultiple: same shape as executeByteCode but with one
    // method invoked N times with N parameter sets. paramTagsFlat etc. is
    // a flat concat of all param Variants across calls; paramGroupSizes[i]
    // is the number of params in call i. Sum(paramGroupSizes) == flat array
    // length. Returns one FullSetterResult per call.
    public static FullExecuteResult executeByteCodeMultipleFull(
            long accessId,
            byte[] initiatorAddress,
            byte[] contractAddress,
            String[] classNames, byte[][] byteCodes,
            byte[] instance, boolean stateCanModify,
            String methodName,
            int[] paramGroupSizes,
            int[]     paramTagsFlat,
            boolean[] paramBoolsFlat,
            long[]    paramLongsFlat,
            double[]  paramDoublesFlat,
            String[]  paramStringsFlat,
            byte[][]  paramBytesFlat,
            long executionTime,
            short version) {
        try {
            ContractExecutorHandler handler = getFullHandler();

            // Build SmartContractBinary (same as single-method path).
            List<ByteCodeObject> bcs = new ArrayList<>(classNames.length);
            for (int i = 0; i < classNames.length; ++i) {
                ByteCodeObject obj = new ByteCodeObject();
                obj.name = classNames[i];
                obj.byteCode = ByteBuffer.wrap(byteCodes[i] != null ? byteCodes[i] : new byte[0]);
                bcs.add(obj);
            }
            ClassObject classObject = new ClassObject();
            classObject.byteCodeObjects = bcs;
            classObject.instance = ByteBuffer.wrap(instance != null ? instance : new byte[0]);

            SmartContractBinary scb = new SmartContractBinary();
            scb.contractAddress = ByteBuffer.wrap(contractAddress != null ? contractAddress : new byte[32]);
            scb.object = classObject;
            scb.setStateCanModify(stateCanModify);

            // Slice the flat param arrays into per-call lists.
            List<List<Variant>> paramsMatrix = new ArrayList<>();
            int cursor = 0;
            for (int callIdx = 0; callIdx < paramGroupSizes.length; ++callIdx) {
                int sz = paramGroupSizes[callIdx];
                List<Variant> callParams = new ArrayList<>(sz);
                for (int j = 0; j < sz; ++j) {
                    int idx = cursor + j;
                    callParams.add(buildVariantFromTagged(
                        paramTagsFlat[idx],
                        paramBoolsFlat   != null && idx < paramBoolsFlat.length   ? paramBoolsFlat[idx]   : false,
                        paramLongsFlat   != null && idx < paramLongsFlat.length   ? paramLongsFlat[idx]   : 0L,
                        paramDoublesFlat != null && idx < paramDoublesFlat.length ? paramDoublesFlat[idx] : 0.0,
                        paramStringsFlat != null && idx < paramStringsFlat.length ? paramStringsFlat[idx] : null,
                        paramBytesFlat   != null && idx < paramBytesFlat.length   ? paramBytesFlat[idx]   : null));
                }
                paramsMatrix.add(callParams);
                cursor += sz;
            }

            ByteBuffer initiator = ByteBuffer.wrap(initiatorAddress != null ? initiatorAddress : new byte[32]);

            com.credits.client.executor.thrift.generated.ExecuteByteCodeMultipleResult r =
                handler.executeByteCodeMultiple(accessId, initiator, scb,
                                                methodName != null ? methodName : "",
                                                paramsMatrix, executionTime, version);

            byte code     = (r.status != null) ? r.status.code : (byte) -1;
            String message= (r.status != null && r.status.message != null) ? r.status.message : "";

            // ExecuteByteCodeMultipleResult.results is List<GetterMethodResult>;
            // shape is similar but field names differ. Repack to FullSetterResult.
            FullSetterResult[] outResults;
            if (r.results != null) {
                outResults = new FullSetterResult[r.results.size()];
                for (int i = 0; i < r.results.size(); ++i) {
                    com.credits.client.executor.thrift.generated.GetterMethodResult gr = r.results.get(i);
                    byte sCode  = (gr.status != null) ? gr.status.code : (byte) 0;
                    String sMsg = (gr.status != null && gr.status.message != null) ? gr.status.message : "";
                    TaggedVariant ret = flattenVariant(gr.ret_val);
                    // GetterMethodResult has no contractsState/emittedTransactions/executionCost.
                    outResults[i] = new FullSetterResult(sCode, sMsg, ret,
                                                        new FullStateEntry[0],
                                                        new FullEmittedTxn[0],
                                                        0L);
                }
            } else {
                outResults = new FullSetterResult[0];
            }
            return new FullExecuteResult(code, message, outResults);
        } catch (Throwable t) {
            return new FullExecuteResult((byte) 1, "bridge: " + t.toString(), new FullSetterResult[0]);
        }
    }

    private static byte[] bbToBytes(ByteBuffer bb) {
        if (bb == null) return new byte[0];
        ByteBuffer dup = bb.duplicate();
        byte[] out = new byte[dup.remaining()];
        dup.get(out);
        return out;
    }

    // Synthetic Variant round-trip — constructs Variants of each supported
    // tag and returns the flattened TaggedVariant array. Validates the
    // unwrap pipeline without needing real contract state.
    public static TaggedVariant[] makeVariantSamples() {
        Variant vBool   = new Variant(); vBool.setV_boolean(true);
        Variant vInt    = new Variant(); vInt.setV_int(42);
        Variant vLong   = new Variant(); vLong.setV_long(9_999_999_999L);
        Variant vDouble = new Variant(); vDouble.setV_double(3.14159);
        Variant vString = new Variant(); vString.setV_string("variant-string");
        Variant vBytes  = new Variant(); vBytes.setV_byte_array(new byte[]{1, 2, 3, 4});
        Variant vNull   = new Variant(); vNull.setV_null("");
        // V_LIST is a recursive case — flatten will report VTAG_OTHER.
        java.util.ArrayList<Variant> inner = new java.util.ArrayList<>();
        inner.add(vInt);
        Variant vList = new Variant(); vList.setV_list(inner);

        Variant[] in = { vBool, vInt, vLong, vDouble, vString, vBytes, vNull, vList };
        TaggedVariant[] out = new TaggedVariant[in.length];
        for (int i = 0; i < in.length; ++i) out[i] = flattenVariant(in[i]);
        return out;
    }

    public static MethodsResult getContractMethods(String[] classNames, byte[][] byteCodes, short version) {
        try {
            if (classNames == null || byteCodes == null || classNames.length != byteCodes.length) {
                return new MethodsResult((byte) 1, "bridge: bad input arrays", new MethodInfo[0]);
            }
            ContractExecutorHandler handler = getFullHandler();

            List<ByteCodeObject> in = new ArrayList<>(classNames.length);
            for (int i = 0; i < classNames.length; ++i) {
                ByteCodeObject obj = new ByteCodeObject();
                obj.name = classNames[i];
                obj.byteCode = ByteBuffer.wrap(byteCodes[i] != null ? byteCodes[i] : new byte[0]);
                in.add(obj);
            }
            GetContractMethodsResult r = handler.getContractMethods(in, version);

            byte code = (r.status != null) ? r.status.code : (byte) -1;
            String message = (r.status != null && r.status.message != null) ? r.status.message : "";
            MethodInfo[] outArr;
            if (r.methods != null) {
                outArr = new MethodInfo[r.methods.size()];
                for (int i = 0; i < r.methods.size(); ++i) {
                    MethodDescription md = r.methods.get(i);
                    String name = md.name != null ? md.name : "";
                    String rtype = md.returnType != null ? md.returnType : "";
                    outArr[i] = new MethodInfo(name, rtype,
                                               flattenArgs(md.arguments),
                                               flattenAnnotations(md.annotations));
                }
            } else {
                outArr = new MethodInfo[0];
            }
            return new MethodsResult(code, message, outArr);
        } catch (Throwable t) {
            return new MethodsResult((byte) 1, "bridge: " + t.toString(), new MethodInfo[0]);
        }
    }

    public static CompileResult compileSourceCode(String sourceCode, short version) {
        try {
            ContractExecutorHandler handler = getFullHandler();
            CompileSourceCodeResult r = handler.compileSourceCode(sourceCode, version);

            byte code = (r.status != null) ? r.status.code : (byte) -1;
            String message = (r.status != null && r.status.message != null) ? r.status.message : "";
            CompiledClass[] cls;
            if (r.byteCodeObjects != null) {
                List<ByteCodeObject> list = r.byteCodeObjects;
                cls = new CompiledClass[list.size()];
                for (int i = 0; i < list.size(); ++i) {
                    ByteCodeObject bo = list.get(i);
                    String name = bo.name != null ? bo.name : "";
                    byte[] bytes;
                    if (bo.byteCode != null) {
                        ByteBuffer bb = bo.byteCode.duplicate();
                        bytes = new byte[bb.remaining()];
                        bb.get(bytes);
                    } else {
                        bytes = new byte[0];
                    }
                    cls[i] = new CompiledClass(name, bytes);
                }
            } else {
                cls = new CompiledClass[0];
            }
            return new CompileResult(code, message, cls);
        } catch (Throwable t) {
            return new CompileResult((byte) 1, "bridge: " + t.toString(), new CompiledClass[0]);
        }
    }
}
