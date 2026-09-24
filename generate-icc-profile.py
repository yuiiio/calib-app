#!/usr/bin/env python3
"""
Sharp LQ140M1JW62 (Dell 3DNW3 / 6-bit FRC) パネル専用 1D LUT 生成スクリプト
"""

import math
import subprocess
import os

LUT_ENTRIES = 256

# EDID より抽出した物理座標
RED_XY   = (0.5898, 0.3544)
GREEN_XY = (0.3457, 0.5449)
BLUE_XY  = (0.1542, 0.0996)
WHITE_XY = (0.3125, 0.3281)

# パネル最適化定数 (Sharp LQ140M1 専用)
GAMMA_TARGET = 2.05
GAMMA_REF = 2.20
GAMMA_P = GAMMA_TARGET / GAMMA_REF  # 0.931818...

def calculate_lut_curves(entries=256):
    r_curve, g_curve, b_curve = [], [], []

    for i in range(entries):
        x = i / (entries - 1)

        # 1. 白黒コントラストを高めるベースガンマ
        base_y = math.pow(x, 1.0 / GAMMA_P) if x > 0 else 0.0

        # 2. LQ140M1JW62 用 R/G ゲイン
        r_gain = 1.000
        g_gain = 0.978  # 緑かぶりを抑制

        # 3. 6-bit FRC に配慮したマイルドな Dynamic B-Gain
        if x < 0.12:
            b_gain = 0.870
        elif x < 0.45:
            # 中間調の青沈みを補正 (Peak 0.932)
            t = (x - 0.12) / 0.33
            b_gain = 0.870 + t * (0.932 - 0.870)
        elif x < 0.78:
            # ハイライトへ向けてスムーズに減衰
            t = (x - 0.45) / 0.33
            b_gain = 0.932 - t * (0.932 - 0.875)
        elif x < 0.85:
            t = (x - 0.78) / 0.07
            b_gain = 0.875 - t * (0.875 - 0.870)
        else:
            b_gain = 0.870

        r_curve.append(min(1.0, max(0.0, base_y * r_gain)))
        g_curve.append(min(1.0, max(0.0, base_y * g_gain)))
        b_curve.append(min(1.0, max(0.0, base_y * b_gain)))

    return r_curve, g_curve, b_curve

def write_cal_file(filename, r_curve, g_curve, b_curve):
    entries = len(r_curve)
    with open(filename, "w", encoding="utf-8") as f:
        f.write("CAL\n")
        f.write('DESCRIPTOR "Sharp LQ140M1 Custom Calibration"\n')
        f.write('KEYWORD "DEVICE_CLASS"\n')
        f.write('DEVICE_CLASS "DISPLAY"\n')
        f.write("NUMBER_OF_FIELDS 4\n")
        f.write("BEGIN_DATA_FORMAT\n")
        f.write("RGB_I RGB_R RGB_G RGB_B\n")
        f.write("END_DATA_FORMAT\n")
        f.write(f"NUMBER_OF_SETS {entries}\n")
        f.write("BEGIN_DATA\n")
        
        for i in range(entries):
            x = i / (entries - 1)
            f.write(f"{x:.6f} {r_curve[i]:.6f} {g_curve[i]:.6f} {b_curve[i]:.6f}\n")
            
        f.write("END_DATA\n")
    print(f"[+] Successfully generated ArgyllCMS .cal file: {filename}")


def build_icc_with_argyll(cal_file, prefix="Sharp_LQ140M1"):
    ti3_file = f"{prefix}.ti3"
    icc_file = f"{prefix}_Calibrated.icc"

    def xy_to_XYZ(x, y, Y=1.0):
        if y == 0: return 0, 0, 0
        X = (x / y) * Y
        Z = ((1.0 - x - y) / y) * Y
        return X, Y, Z

    rX, rY, rZ = xy_to_XYZ(*RED_XY, Y=0.2126)
    gX, gY, gZ = xy_to_XYZ(*GREEN_XY, Y=0.7152)
    bX, bY, bZ = xy_to_XYZ(*BLUE_XY, Y=0.0722)
    wX, wY, wZ = xy_to_XYZ(*WHITE_XY, Y=1.0000)

    # colprof が要求する DEVICE_CLASS ヘッダー項目を追加
    with open(ti3_file, "w", encoding="utf-8") as f:
        f.write("CTI3\n")
        f.write(f'DESCRIPTOR "{prefix} Dummy Profile Data"\n')
        f.write('KEYWORD "DEVICE_CLASS"\n')
        f.write('DEVICE_CLASS "DISPLAY"\n')
        f.write('KEYWORD "COLOR_REP"\n')
        f.write('COLOR_REP "RGB_XYZ"\n')
        f.write("NUMBER_OF_FIELDS 7\n")
        f.write("BEGIN_DATA_FORMAT\n")
        f.write("RGB_R RGB_G RGB_B XYZ_X XYZ_Y XYZ_Z SAMPLE_ID\n")
        f.write("END_DATA_FORMAT\n")
        f.write("NUMBER_OF_SETS 5\n")
        f.write("BEGIN_DATA\n")
        f.write(f"0.0 0.0 0.0 0.0 0.0 0.0 1\n")
        f.write(f"100.0 0.0 0.0 {rX*100:.4f} {rY*100:.4f} {rZ*100:.4f} 2\n")
        f.write(f"0.0 100.0 0.0 {gX*100:.4f} {gY*100:.4f} {gZ*100:.4f} 3\n")
        f.write(f"0.0 0.0 100.0 {bX*100:.4f} {bY*100:.4f} {bZ*100:.4f} 4\n")
        f.write(f"100.0 100.0 100.0 {wX*100:.4f} {wY*100:.4f} {wZ*100:.4f} 5\n")
        f.write("END_DATA\n")

    try:
        cmd = [
            "colprof",
            "-v",
            "-A", "Sharp",
            "-M", "LQ140M1",
            "-D", "Sharp LQ140M1 Calibrated D65",
            "-q", "m",
            "-as",
            "-s", cal_file,
            prefix
        ]
        subprocess.run(cmd, check=True)
        
        generated_icc = f"{prefix}.icc"
        if os.path.exists(generated_icc):
            os.rename(generated_icc, icc_file)
            print(f"[+] Successfully created ICC profile via colprof: {icc_file}")

        if os.path.exists(ti3_file):
            os.remove(ti3_file)

    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        print(f"[!] ICC generation failed using colprof: {e}")


def main():
    prefix = "Sharp_LQ140M1"
    cal_filename = f"{prefix}.cal"

    r_curve, g_curve, b_curve = calculate_lut_curves(LUT_ENTRIES)
    write_cal_file(cal_filename, r_curve, g_curve, b_curve)
    build_icc_with_argyll(cal_filename, prefix)


if __name__ == "__main__":
    main()
