// Spike target: trivial Java class invoked from C++ via JNI.
// Verifies static method dispatch, string return, and a parameter round-trip.
public class HelloEmbed {
    public static String hello() {
        return "hello from embedded jvm";
    }

    public static String echo(String input) {
        return "echo:" + input;
    }

    public static int add(int a, int b) {
        return a + b;
    }

    // Validates byte[] round-tripping. Returns the input reversed.
    // Real executor wrappers will pass contract bytecode and state as byte[].
    public static byte[] reverseBytes(byte[] in) {
        if (in == null) return new byte[0];
        byte[] out = new byte[in.length];
        for (int i = 0; i < in.length; ++i) {
            out[i] = in[in.length - 1 - i];
        }
        return out;
    }
}
