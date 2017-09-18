import java.util.Map;

public class Main {
    static public void main(String[] args) throws Exception {
        checkManager();
        for (int i = 1; i <= 2; i++) {
            System.out.println("\nspawning child #" + i);
            child();
            Thread.sleep(2000);
            checkManager();
        }
        System.out.println("\ndone!");
    }

    static private void child() throws Exception {
        System.out.println("spawning child");
        ProcessBuilder pb = new ProcessBuilder("sleep", "5");
        Process proc = pb.start();
        Thread.sleep(250);
        checkManager();
        proc.waitFor();
        System.out.println("child died");
    }

    static private void checkManager() {
        Map<Thread, StackTraceElement[]> traces = Thread.getAllStackTraces();
        boolean found = false;

        for (Map.Entry<Thread, StackTraceElement[]> entry :
                 traces.entrySet()) {
            Thread t = entry.getKey();
            String name = t.getName();
            if (name.indexOf("process reaper") >= 0) {
                Thread.State state = t.getState();
                System.out.println("process manager: " + state);
                if (state != Thread.State.RUNNABLE && state != Thread.State.TIMED_WAITING) {
                    for (StackTraceElement e : entry.getValue()) {
                        System.out.println("  " + e);
                    }
                }
                found = true;
            }
        }

        if (! found) {
            System.out.println("process manager: nonexistent");
        }
    }
}
