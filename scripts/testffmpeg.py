
import os
import time
from ffmpeg_quality_metrics import FfmpegQualityMetrics as ffqm
import pickle


options = {}



for crf in [14,15,16,17,18,19,20]:
    for preset in ["fast", "medium"]:
        options[f"h264_nvenc_{crf}_{preset}"] = f"-c:v h264_nvenc -cq {crf} -preset:v {preset} -rc:v vbr -b:v 0"

for crf in [14,15,16,17,18,19,20]:
    for preset in ["fast", "medium"]:
        options[f"hevc_nvenc_{crf}_{preset}"] = f"-c:v hevc_nvenc -cq {crf} -preset:v {preset} -rc:v vbr -b:v 0"

for crf in [14,15,16,17,18,19,20]:
    for preset in ["veryfast", "superfast", "faster", "fast"]:
        options[f"libx264_{crf}_{preset}"] = f" -threads 10 -c:v libx264 -crf {crf} -preset {preset}"



elTimes = []
fSizes = []
quals = []
for name, option in options.items():

    if os.path.isfile(f"testoutput/bm_{name}.pickle"):
        continue

    command = f"ffmpeg  -y -i lossless/visss_visss_trigger_S1145792_20211223-150621_0.mov {option} testoutput/bm_{name}.mkv"
    print(name, option, command)

    t1 = time.time()  
    os.system(command)
    t2 = time.time()  

    elTime = t2-t1
    fSize = os.path.getsize(f"testoutput/bm_{name}.mkv")

    elTimes.append(elTime)
    fSizes.append(fSize)
    # qual = ffqm("lossless/visss_visss_trigger_S1145792_20211223-150621_0.mov", f"testoutput/bm_{name}.mkv")
    # qual.calc(["ssim", "psnr"])

    # qual.get_global_stats()
    # quals.append(qual)

    result = [option,elTime,fSize]

    with open(f"testoutput/bm_{name}.pickle", 'wb') as handle:
        pickle.dump(result, handle, protocol=pickle.HIGHEST_PROTOCOL)
