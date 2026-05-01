// Stub mimicking the shape of the real Thrift ContractExecutor service for
// the purposes of validating the JNI wrapper pattern end-to-end.
//
// Mirrors:
//   ExecutorBuildVersionResult {
//       APIResponse status { i8 code; string message }
//       i32 commitNumber
//       string commitHash
//   }
//   getExecutorBuildVersion(i16 version) -> ExecutorBuildVersionResult
//
// The real production wrapper will call into the actual executor JAR's
// service implementation; this stub validates the marshalling contract
// without that dependency.

public class StubExecutor {
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
        // Echo the version back inside the message so the test can verify the
        // i16 parameter actually crossed the boundary correctly.
        final String msg = "stub-ok v=" + version;
        return new BuildVersionResult((byte) 0, msg, 12345, "abc123def456");
    }
}
