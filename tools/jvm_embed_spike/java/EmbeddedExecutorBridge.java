// Bridge between the JNI host and the REAL contract-executor.jar's
// ContractExecutorHandler. Production use would build the full Dagger
// DI graph; this minimal bridge constructs only what getExecutorBuildVersion
// actually touches (ApplicationProperties; ceService is unused on this path).
//
// First end-to-end JNI call against the actual executor JAR — proves the
// integration pattern works against production code, not just a stub.

import com.credits.ApplicationProperties;
import com.credits.thrift.ContractExecutorHandler;
import com.credits.client.executor.thrift.generated.ExecutorBuildVersionResult;

public class EmbeddedExecutorBridge {

    // Mirrors StubExecutor.BuildVersionResult so the C++ side can reuse the
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

    public static BuildVersionResult getExecutorBuildVersion(short version) {
        try {
            ApplicationProperties props = new ApplicationProperties();
            ContractExecutorHandler handler = new ContractExecutorHandler(null, props);
            ExecutorBuildVersionResult r = handler.getExecutorBuildVersion(version);

            byte code = (r.status != null) ? r.status.code : (byte) -1;
            String message = (r.status != null && r.status.message != null) ? r.status.message : "";
            String hash = (r.commitHash != null) ? r.commitHash : "";
            return new BuildVersionResult(code, message, r.commitNumber, hash);
        } catch (Throwable t) {
            // Surface failures into the result instead of letting the throw
            // cross the JNI boundary — the C++ side just sees code != 0.
            return new BuildVersionResult((byte) 1, "bridge: " + t.toString(), 0, "");
        }
    }
}
