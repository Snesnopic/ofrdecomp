
uint FUN_000057f0(long param_1,double *param_2,uint *param_3)

{
  int *piVar1;
  long *plVar2;
  byte *pbVar3;
  long lVar4;
  uint uVar5;
  uint uVar6;
  uint uVar7;
  undefined8 uVar8;
  byte bVar9;
  uint uVar10;
  int iVar11;
  uint uVar12;
  int iVar13;
  ulong uVar14;
  uint uVar15;
  sbyte sVar16;
  bool bVar17;
  
  uVar6 = (uint)((ulong)(long)*param_2 >> 2) & 0x3fffffff;
  uVar15 = uVar6 + 1;
  if (uVar15 < 0x4001) {
    uVar12 = *(uint *)(param_1 + 0x3c);
    uVar5 = param_3[1];
    if (uVar5 < 0x800001) {
      uVar10 = *param_3;
      bVar9 = (byte)param_3[3];
      do {
        uVar10 = (uint)(byte)(bVar9 << 7) | uVar10 << 8;
        *param_3 = uVar10;
        plVar2 = *(long **)(param_3 + 4);
        iVar13 = (int)plVar2[1];
        if (iVar13 == 0) {
          FUN_00003e90(plVar2,1);
          iVar13 = (int)plVar2[1];
          uVar10 = *param_3;
          uVar5 = param_3[1];
        }
        *(int *)(plVar2 + 1) = iVar13 + -1;
        pbVar3 = (byte *)*plVar2;
        *plVar2 = (long)(pbVar3 + 1);
        bVar9 = *pbVar3;
        *(byte *)(param_3 + 3) = bVar9;
        uVar10 = bVar9 >> 1 | uVar10;
        *param_3 = uVar10;
        uVar5 = uVar5 << 8;
        param_3[1] = uVar5;
      } while (uVar5 < 0x800001);
    }
    else {
      uVar10 = *param_3;
    }
    uVar7 = 0;
    param_3[2] = (uint)((ulong)uVar5 / (ulong)uVar12);
    uVar10 = (uint)((ulong)uVar10 / ((ulong)uVar5 / (ulong)uVar12));
    uVar5 = uVar12 - 1;
    if (uVar10 < uVar12) {
      uVar5 = uVar10;
    }
    lVar4 = *(long *)(param_1 + 0x30);
    uVar14 = 1;
    do {
      iVar13 = *(int *)(lVar4 + uVar14 * 4);
      uVar12 = iVar13 + uVar7;
      iVar11 = (int)uVar14;
      if (uVar5 < uVar12) {
        *(int *)(lVar4 + uVar14 * 4) = iVar13 + 2;
        iVar13 = iVar11;
      }
      else {
        iVar13 = iVar11 + 1;
        uVar7 = uVar12;
      }
      uVar12 = iVar11 + iVar13;
      uVar14 = (ulong)uVar12;
    } while (uVar12 < *(uint *)(param_1 + 0x44));
    iVar13 = *(int *)(lVar4 + uVar14 * 4);
    iVar11 = param_3[2] * uVar7;
    uVar5 = *(uint *)(param_1 + 0x3c);
    *param_3 = *param_3 - iVar11;
    if (uVar7 + iVar13 < uVar5) {
      uVar5 = param_3[2] * iVar13;
    }
    else {
      uVar5 = param_3[1] - iVar11;
    }
    param_3[1] = uVar5;
    piVar1 = (int *)(lVar4 + uVar14 * 4);
    *piVar1 = *piVar1 + 2;
    uVar5 = *(int *)(param_1 + 0x3c) + 2;
    *(uint *)(param_1 + 0x3c) = uVar5;
    if (*(uint *)(param_1 + 0x40) <= uVar5) {
      FUN_00002df0(param_1 + 0x30);
    }
    iVar13 = uVar12 - *(int *)(param_1 + 0x44);
    if (iVar13 != 0x1f) {
      uVar14 = (ulong)param_3[1];
      if (param_3[1] < 0x800001) {
        uVar12 = *param_3;
        bVar9 = (byte)param_3[3];
        do {
          uVar5 = (uint)uVar14;
          uVar12 = (uint)(byte)(bVar9 << 7) | uVar12 << 8;
          *param_3 = uVar12;
          plVar2 = *(long **)(param_3 + 4);
          iVar11 = (int)plVar2[1];
          if (iVar11 == 0) {
            FUN_00003e90(plVar2,1);
            iVar11 = (int)plVar2[1];
            uVar12 = *param_3;
            uVar5 = param_3[1];
          }
          *(int *)(plVar2 + 1) = iVar11 + -1;
          pbVar3 = (byte *)*plVar2;
          *plVar2 = (long)(pbVar3 + 1);
          bVar9 = *pbVar3;
          *(byte *)(param_3 + 3) = bVar9;
          uVar12 = bVar9 >> 1 | uVar12;
          *param_3 = uVar12;
          uVar5 = uVar5 << 8;
          uVar14 = (ulong)uVar5;
          param_3[1] = uVar5;
        } while (uVar5 < 0x800001);
      }
      else {
        uVar12 = *param_3;
      }
      uVar10 = (uint)(uVar14 / uVar15);
      param_3[2] = uVar10;
      uVar5 = (uint)((ulong)uVar12 / (uVar14 / uVar15));
      if (uVar5 < uVar15) {
        uVar6 = uVar5;
      }
      *param_3 = uVar12 - uVar6 * uVar10;
      uVar12 = (int)uVar14 - uVar6 * uVar10;
      if (uVar6 + 1 < uVar15) {
        uVar12 = uVar10;
      }
      param_3[1] = uVar12;
      uVar6 = iVar13 * uVar15 + uVar6;
      goto LAB_00005e28;
    }
    uVar15 = uVar15 * 0x1f;
    uVar6 = uVar15 >> 0x10;
    if (uVar15 < 0x10000) {
      uVar6 = uVar15;
    }
    uVar15 = (uint)(0xffff < uVar15) << 4;
    if (0xff < uVar6) {
      uVar6 = uVar6 >> 8;
      uVar15 = uVar15 | 8;
    }
    if (0xf < uVar6) {
      uVar6 = uVar6 >> 4;
      uVar15 = uVar15 + 4;
    }
    if (3 < uVar6) {
      uVar6 = uVar6 >> 2;
      uVar15 = uVar15 + 2;
    }
    uVar15 = (1 < uVar6) + uVar15;
    uVar6 = *(uint *)(param_1 + 0x28) - uVar15;
    if (*(uint *)(param_1 + 0x28) < uVar15 || uVar6 == 0) {
      uVar8 = ___cxa_allocate_exception(0x28);
      FUN_000032f0(uVar8,"unknown",0,"none");
Subroutine does not return */
      ___cxa_throw(uVar8,&PTR___ZTVN10__cxxabiv120__si_class_type_infoE_00021fe0,FUN_00003320);
    }
    uVar14 = (ulong)param_3[1];
    if (param_3[1] < 0x800001) {
      uVar12 = *param_3;
      bVar9 = (byte)param_3[3];
      do {
        uVar5 = (uint)uVar14;
        uVar12 = (uint)(byte)(bVar9 << 7) | uVar12 << 8;
        *param_3 = uVar12;
        plVar2 = *(long **)(param_3 + 4);
        iVar13 = (int)plVar2[1];
        if (iVar13 == 0) {
          FUN_00003e90(plVar2,1);
          iVar13 = (int)plVar2[1];
          uVar12 = *param_3;
          uVar5 = param_3[1];
        }
        *(int *)(plVar2 + 1) = iVar13 + -1;
        pbVar3 = (byte *)*plVar2;
        *plVar2 = (long)(pbVar3 + 1);
        bVar9 = *pbVar3;
        *(byte *)(param_3 + 3) = bVar9;
        uVar12 = bVar9 >> 1 | uVar12;
        *param_3 = uVar12;
        uVar5 = uVar5 << 8;
        uVar14 = (ulong)uVar5;
        param_3[1] = uVar5;
      } while (uVar5 < 0x800001);
    }
    else {
      uVar12 = *param_3;
    }
    uVar5 = (uint)(uVar14 / uVar6);
    param_3[2] = uVar5;
    uVar7 = (uint)((ulong)uVar12 / (uVar14 / uVar6));
    uVar10 = uVar6 - 1;
    if (uVar7 < uVar6) {
      uVar10 = uVar7;
    }
    *param_3 = uVar12 - uVar10 * uVar5;
    uVar12 = (int)uVar14 - uVar10 * uVar5;
    bVar17 = uVar10 + 1 < uVar6;
  }
  else {
    uVar6 = uVar15 >> 0x10;
    if (uVar15 < 0x10000) {
      uVar6 = uVar15;
    }
    bVar9 = (0xffff < uVar15) << 4;
    if (0xff < uVar6) {
      uVar6 = uVar6 >> 8;
      bVar9 = bVar9 | 8;
    }
    if (0xf < uVar6) {
      uVar6 = uVar6 >> 4;
      bVar9 = bVar9 + 4;
    }
    if (3 < uVar6) {
      uVar6 = uVar6 >> 2;
      bVar9 = bVar9 + 2;
    }
    sVar16 = (1 < uVar6) + bVar9;
    uVar6 = *(uint *)(param_1 + 0x3c);
    uVar15 = param_3[1];
    if (uVar15 < 0x800001) {
      uVar12 = *param_3;
      bVar9 = (byte)param_3[3];
      do {
        uVar12 = (uint)(byte)(bVar9 << 7) | uVar12 << 8;
        *param_3 = uVar12;
        plVar2 = *(long **)(param_3 + 4);
        iVar13 = (int)plVar2[1];
        if (iVar13 == 0) {
          FUN_00003e90(plVar2,1);
          iVar13 = (int)plVar2[1];
          uVar12 = *param_3;
          uVar15 = param_3[1];
        }
        *(int *)(plVar2 + 1) = iVar13 + -1;
        pbVar3 = (byte *)*plVar2;
        *plVar2 = (long)(pbVar3 + 1);
        bVar9 = *pbVar3;
        *(byte *)(param_3 + 3) = bVar9;
        uVar12 = bVar9 >> 1 | uVar12;
        *param_3 = uVar12;
        uVar15 = uVar15 << 8;
        param_3[1] = uVar15;
      } while (uVar15 < 0x800001);
    }
    else {
      uVar12 = *param_3;
    }
    uVar5 = 0;
    param_3[2] = (uint)((ulong)uVar15 / (ulong)uVar6);
    uVar12 = (uint)((ulong)uVar12 / ((ulong)uVar15 / (ulong)uVar6));
    uVar15 = uVar6 - 1;
    if (uVar12 < uVar6) {
      uVar15 = uVar12;
    }
    lVar4 = *(long *)(param_1 + 0x30);
    uVar14 = 1;
    do {
      iVar13 = *(int *)(lVar4 + uVar14 * 4);
      uVar6 = iVar13 + uVar5;
      iVar11 = (int)uVar14;
      if (uVar15 < uVar6) {
        *(int *)(lVar4 + uVar14 * 4) = iVar13 + 2;
        iVar13 = iVar11;
      }
      else {
        iVar13 = iVar11 + 1;
        uVar5 = uVar6;
      }
      uVar6 = iVar11 + iVar13;
      uVar14 = (ulong)uVar6;
    } while (uVar6 < *(uint *)(param_1 + 0x44));
    iVar13 = *(int *)(lVar4 + uVar14 * 4);
    iVar11 = param_3[2] * uVar5;
    uVar15 = *(uint *)(param_1 + 0x3c);
    *param_3 = *param_3 - iVar11;
    if (uVar5 + iVar13 < uVar15) {
      uVar15 = param_3[2] * iVar13;
    }
    else {
      uVar15 = param_3[1] - iVar11;
    }
    param_3[1] = uVar15;
    piVar1 = (int *)(lVar4 + uVar14 * 4);
    *piVar1 = *piVar1 + 2;
    uVar15 = *(int *)(param_1 + 0x3c) + 2;
    *(uint *)(param_1 + 0x3c) = uVar15;
    if (*(uint *)(param_1 + 0x40) <= uVar15) {
      FUN_00002df0(param_1 + 0x30);
    }
    iVar13 = uVar6 - *(int *)(param_1 + 0x44);
    if (iVar13 != 0x1f) {
      iVar11 = FUN_000055f0(param_3,sVar16);
      uVar6 = iVar11 + (iVar13 << sVar16);
      goto LAB_00005e28;
    }
    uVar15 = 0x1f << sVar16;
    uVar6 = uVar15 >> 0x10;
    if (uVar15 < 0x10000) {
      uVar6 = uVar15;
    }
    uVar15 = (uint)(0xffff < uVar15) << 4;
    if (0xff < uVar6) {
      uVar6 = uVar6 >> 8;
      uVar15 = uVar15 | 8;
    }
    if (0xf < uVar6) {
      uVar6 = uVar6 >> 4;
      uVar15 = uVar15 + 4;
    }
    if (3 < uVar6) {
      uVar6 = uVar6 >> 2;
      uVar15 = uVar15 + 2;
    }
    uVar15 = (1 < uVar6) + uVar15;
    uVar6 = *(uint *)(param_1 + 0x28) - uVar15;
    if (*(uint *)(param_1 + 0x28) < uVar15 || uVar6 == 0) {
      uVar8 = ___cxa_allocate_exception(0x28);
      FUN_000032f0(uVar8,"unknown",0,"none");
      goto LAB_00005ebd;
    }
    uVar14 = (ulong)param_3[1];
    if (param_3[1] < 0x800001) {
      uVar12 = *param_3;
      bVar9 = (byte)param_3[3];
      do {
        uVar5 = (uint)uVar14;
        uVar12 = (uint)(byte)(bVar9 << 7) | uVar12 << 8;
        *param_3 = uVar12;
        plVar2 = *(long **)(param_3 + 4);
        iVar13 = (int)plVar2[1];
        if (iVar13 == 0) {
          FUN_00003e90(plVar2,1);
          iVar13 = (int)plVar2[1];
          uVar12 = *param_3;
          uVar5 = param_3[1];
        }
        *(int *)(plVar2 + 1) = iVar13 + -1;
        pbVar3 = (byte *)*plVar2;
        *plVar2 = (long)(pbVar3 + 1);
        bVar9 = *pbVar3;
        *(byte *)(param_3 + 3) = bVar9;
        uVar12 = bVar9 >> 1 | uVar12;
        *param_3 = uVar12;
        uVar5 = uVar5 << 8;
        uVar14 = (ulong)uVar5;
        param_3[1] = uVar5;
      } while (uVar5 < 0x800001);
    }
    else {
      uVar12 = *param_3;
    }
    uVar5 = (uint)(uVar14 / uVar6);
    param_3[2] = uVar5;
    uVar7 = (uint)((ulong)uVar12 / (uVar14 / uVar6));
    uVar10 = uVar6 - 1;
    if (uVar7 < uVar6) {
      uVar10 = uVar7;
    }
    *param_3 = uVar12 - uVar10 * uVar5;
    uVar12 = (int)uVar14 - uVar10 * uVar5;
    bVar17 = uVar10 + 1 < uVar6;
  }
  if (bVar17) {
    uVar12 = uVar5;
  }
  param_3[1] = uVar12;
  iVar13 = FUN_000055f0(param_3,uVar10 + uVar15);
  uVar6 = (1 << ((byte)(uVar10 + uVar15) & 0x1f)) + iVar13;
  *param_2 = (double)uVar6 * *(double *)(param_1 + 0x10) + *param_2 * *(double *)(param_1 + 8);
  return (int)(uVar6 << 0x1f) >> 0x1f ^ uVar6 >> 1;
}

