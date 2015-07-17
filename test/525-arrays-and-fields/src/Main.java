/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// Test on (in)variant static and instance field and array references in loops.
//
public class Main {

  private static Object anObject = new Object();
  private static Object anotherObject = new Object();

  //
  // Static fields.
  //

  private static boolean sZ;
  private static byte sB;
  private static char sC;
  private static short sS;
  private static int sI;
  private static long sJ;
  private static float sF;
  private static double sD;
  private static Object sL;

  //
  // Static arrays.
  //

  private static boolean[] sArrZ;
  private static byte[] sArrB;
  private static char[] sArrC;
  private static short[] sArrS;
  private static int[] sArrI;
  private static long[] sArrJ;
  private static float[] sArrF;
  private static double[] sArrD;
  private static Object[] sArrL;

  //
  // Instance fields.
  //

  private boolean mZ;
  private byte mB;
  private char mC;
  private short mS;
  private int mI;
  private long mJ;
  private float mF;
  private double mD;
  private Object mL;

  //
  // Instance arrays.
  //

  private boolean[] mArrZ;
  private byte[] mArrB;
  private char[] mArrC;
  private short[] mArrS;
  private int[] mArrI;
  private long[] mArrJ;
  private float[] mArrF;
  private double[] mArrD;
  private Object[] mArrL;

  //
  // Loops on static arrays with invariant static field references.
  //

  private static void SInvLoopZ() {
    for (int i = 0; i < sArrZ.length; i++) {
      sArrZ[i] = sZ;
    }
  }

  private static void SInvLoopB() {
    for (int i = 0; i < sArrB.length; i++) {
      sArrB[i] = sB;
    }
  }

  private static void SInvLoopC() {
    for (int i = 0; i < sArrC.length; i++) {
      sArrC[i] = sC;
    }
  }

  private static void SInvLoopS() {
    for (int i = 0; i < sArrS.length; i++) {
      sArrS[i] = sS;
    }
  }

  private static void SInvLoopI() {
    for (int i = 0; i < sArrI.length; i++) {
      sArrI[i] = sI;
    }
  }

  private static void SInvLoopJ() {
    for (int i = 0; i < sArrJ.length; i++) {
      sArrJ[i] = sJ;
    }
  }

  private static void SInvLoopF() {
    for (int i = 0; i < sArrF.length; i++) {
      sArrF[i] = sF;
    }
  }

  private static void SInvLoopD() {
    for (int i = 0; i < sArrD.length; i++) {
      sArrD[i] = sD;
    }
  }

  private static void SInvLoopL() {
    for (int i = 0; i < sArrL.length; i++) {
      sArrL[i] = sL;
    }
  }

  //
  // Loops on static arrays with variant static field references.
  //

  private static void SVarLoopZ() {
    for (int i = 0; i < sArrZ.length; i++) {
      sArrZ[i] = sZ;
      if (i == 10)
        sZ = !sZ;
    }
  }

  private static void SVarLoopB() {
    for (int i = 0; i < sArrB.length; i++) {
      sArrB[i] = sB;
      if (i == 10)
        sB++;
    }
  }

  private static void SVarLoopC() {
    for (int i = 0; i < sArrC.length; i++) {
      sArrC[i] = sC;
      if (i == 10)
        sC++;
    }
  }

  private static void SVarLoopS() {
    for (int i = 0; i < sArrS.length; i++) {
      sArrS[i] = sS;
      if (i == 10)
        sS++;
    }
  }

  private static void SVarLoopI() {
    for (int i = 0; i < sArrI.length; i++) {
      sArrI[i] = sI;
      if (i == 10)
        sI++;
    }
  }

  private static void SVarLoopJ() {
    for (int i = 0; i < sArrJ.length; i++) {
      sArrJ[i] = sJ;
      if (i == 10)
        sJ++;
    }
  }

  private static void SVarLoopF() {
    for (int i = 0; i < sArrF.length; i++) {
      sArrF[i] = sF;
      if (i == 10)
        sF++;
    }
  }

  private static void SVarLoopD() {
    for (int i = 0; i < sArrD.length; i++) {
      sArrD[i] = sD;
      if (i == 10)
        sD++;
    }
  }

  private static void SVarLoopL() {
    for (int i = 0; i < sArrL.length; i++) {
      sArrL[i] = sL;
      if (i == 10)
        sL = anotherObject;
    }
  }

  //
  // Loops on instance arrays with invariant instance field references.
  //

  private void InvLoopZ() {
    for (int i = 0; i < mArrZ.length; i++) {
      mArrZ[i] = mZ;
    }
  }

  private void InvLoopB() {
    for (int i = 0; i < mArrB.length; i++) {
      mArrB[i] = mB;
    }
  }

  private void InvLoopC() {
    for (int i = 0; i < mArrC.length; i++) {
      mArrC[i] = mC;
    }
  }

  private void InvLoopS() {
    for (int i = 0; i < mArrS.length; i++) {
      mArrS[i] = mS;
    }
  }

  private void InvLoopI() {
    for (int i = 0; i < mArrI.length; i++) {
      mArrI[i] = mI;
    }
  }

  private void InvLoopJ() {
    for (int i = 0; i < mArrJ.length; i++) {
      mArrJ[i] = mJ;
    }
  }

  private void InvLoopF() {
    for (int i = 0; i < mArrF.length; i++) {
      mArrF[i] = mF;
    }
  }

  private void InvLoopD() {
    for (int i = 0; i < mArrD.length; i++) {
      mArrD[i] = mD;
    }
  }

  private void InvLoopL() {
    for (int i = 0; i < mArrL.length; i++) {
      mArrL[i] = mL;
    }
  }

  //
  // Loops on instance arrays with variant instance field references.
  //

  private void VarLoopZ() {
    for (int i = 0; i < mArrZ.length; i++) {
      mArrZ[i] = mZ;
      if (i == 10)
        mZ = !mZ;
    }
  }

  private void VarLoopB() {
    for (int i = 0; i < mArrB.length; i++) {
      mArrB[i] = mB;
      if (i == 10)
        mB++;
    }
  }

  private void VarLoopC() {
    for (int i = 0; i < mArrC.length; i++) {
      mArrC[i] = mC;
      if (i == 10)
        mC++;
    }
  }

  private void VarLoopS() {
    for (int i = 0; i < mArrS.length; i++) {
      mArrS[i] = mS;
      if (i == 10)
        mS++;
    }
  }

  private void VarLoopI() {
    for (int i = 0; i < mArrI.length; i++) {
      mArrI[i] = mI;
      if (i == 10)
        mI++;
    }
  }

  private void VarLoopJ() {
    for (int i = 0; i < mArrJ.length; i++) {
      mArrJ[i] = mJ;
      if (i == 10)
        mJ++;
    }
  }

  private void VarLoopF() {
    for (int i = 0; i < mArrF.length; i++) {
      mArrF[i] = mF;
      if (i == 10)
        mF++;
    }
  }

  private void VarLoopD() {
    for (int i = 0; i < mArrD.length; i++) {
      mArrD[i] = mD;
      if (i == 10)
        mD++;
    }
  }

  private void VarLoopL() {
    for (int i = 0; i < mArrL.length; i++) {
      mArrL[i] = mL;
      if (i == 10)
        mL = anotherObject;
    }
  }
  //
  // Driver and testers.
  //

  public static void main(String[] args) {
    DoStaticTests();
    new Main().DoInstanceTests();
  }

  private static void DoStaticTests() {
    // Type Z.
    sZ = true;
    sArrZ = new boolean[100];
    SInvLoopZ();
    for (int i = 0; i < sArrZ.length; i++) {
      expectEquals(true, sArrZ[i]);
    }
    SVarLoopZ();
    for (int i = 0; i < sArrZ.length; i++) {
      expectEquals(i <= 10, sArrZ[i]);
    }
    // Type B.
    sB = 1;
    sArrB = new byte[100];
    SInvLoopB();
    for (int i = 0; i < sArrB.length; i++) {
      expectEquals(1, sArrB[i]);
    }
    SVarLoopB();
    for (int i = 0; i < sArrB.length; i++) {
      expectEquals(i <= 10 ? 1 : 2, sArrB[i]);
    }
    // Type C.
    sC = 2;
    sArrC = new char[100];
    SInvLoopC();
    for (int i = 0; i < sArrC.length; i++) {
      expectEquals(2, sArrC[i]);
    }
    SVarLoopC();
    for (int i = 0; i < sArrC.length; i++) {
      expectEquals(i <= 10 ? 2 : 3, sArrC[i]);
    }
    // Type S.
    sS = 3;
    sArrS = new short[100];
    SInvLoopS();
    for (int i = 0; i < sArrS.length; i++) {
      expectEquals(3, sArrS[i]);
    }
    SVarLoopS();
    for (int i = 0; i < sArrS.length; i++) {
      expectEquals(i <= 10 ? 3 : 4, sArrS[i]);
    }
    // Type I.
    sI = 4;
    sArrI = new int[100];
    SInvLoopI();
    for (int i = 0; i < sArrI.length; i++) {
      expectEquals(4, sArrI[i]);
    }
    SVarLoopI();
    for (int i = 0; i < sArrI.length; i++) {
      expectEquals(i <= 10 ? 4 : 5, sArrI[i]);
    }
    // Type J.
    sJ = 5;
    sArrJ = new long[100];
    SInvLoopJ();
    for (int i = 0; i < sArrJ.length; i++) {
      expectEquals(5, sArrJ[i]);
    }
    SVarLoopJ();
    for (int i = 0; i < sArrJ.length; i++) {
      expectEquals(i <= 10 ? 5 : 6, sArrJ[i]);
    }
    // Type F.
    sF = 6.0f;
    sArrF = new float[100];
    SInvLoopF();
    for (int i = 0; i < sArrF.length; i++) {
      expectEquals(6, sArrF[i]);
    }
    SVarLoopF();
    for (int i = 0; i < sArrF.length; i++) {
      expectEquals(i <= 10 ? 6 : 7, sArrF[i]);
    }
    // Type D.
    sD = 7.0;
    sArrD = new double[100];
    SInvLoopD();
    for (int i = 0; i < sArrD.length; i++) {
      expectEquals(7.0, sArrD[i]);
    }
    SVarLoopD();
    for (int i = 0; i < sArrD.length; i++) {
      expectEquals(i <= 10 ? 7 : 8, sArrD[i]);
    }
    // Type L.
    sL = anObject;
    sArrL = new Object[100];
    SInvLoopL();
    for (int i = 0; i < sArrL.length; i++) {
      expectEquals(anObject, sArrL[i]);
    }
    SVarLoopL();
    for (int i = 0; i < sArrL.length; i++) {
      expectEquals(i <= 10 ? anObject : anotherObject, sArrL[i]);
    }
  }

  private void DoInstanceTests() {
    // Type Z.
    mZ = true;
    mArrZ = new boolean[100];
    InvLoopZ();
    for (int i = 0; i < mArrZ.length; i++) {
      expectEquals(true, mArrZ[i]);
    }
    VarLoopZ();
    for (int i = 0; i < mArrZ.length; i++) {
      expectEquals(i <= 10, mArrZ[i]);
    }
    // Type B.
    mB = 1;
    mArrB = new byte[100];
    InvLoopB();
    for (int i = 0; i < mArrB.length; i++) {
      expectEquals(1, mArrB[i]);
    }
    VarLoopB();
    for (int i = 0; i < mArrB.length; i++) {
      expectEquals(i <= 10 ? 1 : 2, mArrB[i]);
    }
    // Type C.
    mC = 2;
    mArrC = new char[100];
    InvLoopC();
    for (int i = 0; i < mArrC.length; i++) {
      expectEquals(2, mArrC[i]);
    }
    VarLoopC();
    for (int i = 0; i < mArrC.length; i++) {
      expectEquals(i <= 10 ? 2 : 3, mArrC[i]);
    }
    // Type S.
    mS = 3;
    mArrS = new short[100];
    InvLoopS();
    for (int i = 0; i < mArrS.length; i++) {
      expectEquals(3, mArrS[i]);
    }
    VarLoopS();
    for (int i = 0; i < mArrS.length; i++) {
      expectEquals(i <= 10 ? 3 : 4, mArrS[i]);
    }
    // Type I.
    mI = 4;
    mArrI = new int[100];
    InvLoopI();
    for (int i = 0; i < mArrI.length; i++) {
      expectEquals(4, mArrI[i]);
    }
    VarLoopI();
    for (int i = 0; i < mArrI.length; i++) {
      expectEquals(i <= 10 ? 4 : 5, mArrI[i]);
    }
    // Type J.
    mJ = 5;
    mArrJ = new long[100];
    InvLoopJ();
    for (int i = 0; i < mArrJ.length; i++) {
      expectEquals(5, mArrJ[i]);
    }
    VarLoopJ();
    for (int i = 0; i < mArrJ.length; i++) {
      expectEquals(i <= 10 ? 5 : 6, mArrJ[i]);
    }
    // Type F.
    mF = 6.0f;
    mArrF = new float[100];
    InvLoopF();
    for (int i = 0; i < mArrF.length; i++) {
      expectEquals(6, mArrF[i]);
    }
    VarLoopF();
    for (int i = 0; i < mArrF.length; i++) {
      expectEquals(i <= 10 ? 6 : 7, mArrF[i]);
    }
    // Type D.
    mD = 7.0;
    mArrD = new double[100];
    InvLoopD();
    for (int i = 0; i < mArrD.length; i++) {
      expectEquals(7.0, mArrD[i]);
    }
    VarLoopD();
    for (int i = 0; i < mArrD.length; i++) {
      expectEquals(i <= 10 ? 7 : 8, mArrD[i]);
    }
    // Type L.
    mL = anObject;
    mArrL = new Object[100];
    InvLoopL();
    for (int i = 0; i < mArrL.length; i++) {
      expectEquals(anObject, mArrL[i]);
    }
    VarLoopL();
    for (int i = 0; i < mArrL.length; i++) {
      expectEquals(i <= 10 ? anObject : anotherObject, mArrL[i]);
    }
  }

  private static void expectEquals(boolean expected, boolean result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  private static void expectEquals(byte expected, byte result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  private static void expectEquals(char expected, char result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  private static void expectEquals(short expected, short result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  private static void expectEquals(int expected, int result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  private static void expectEquals(long expected, long result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  private static void expectEquals(float expected, float result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  private static void expectEquals(double expected, double result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }

  private static void expectEquals(Object expected, Object result) {
    if (expected != result) {
      throw new Error("Expected: " + expected + ", found: " + result);
    }
  }
}
