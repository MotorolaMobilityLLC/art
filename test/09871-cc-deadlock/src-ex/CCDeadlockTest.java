import java.util.Random;

public class CCDeadlockTest implements ICCDeadlockTest {
    public double testLoad() {
        Random rand = new Random();
        int n = rand.nextInt(4);
        return n;
    }

    public double testLoad2() {
        Random rand = new Random();
        double n = rand.nextInt(4) * 0.2;
        return n;
    }
}
