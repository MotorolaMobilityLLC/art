import java.util.Random;

public class CCDeadlockTest2 implements ICCDeadlockTest{
    public double testLoad(){
        Random rand = new Random();
        double n = rand.nextInt(100)*0.3;
        return n;
    }

    public double testLoad2(){
        Random rand = new Random();
        double n = rand.nextInt(10)*0.3;
        return n;
    }
}
