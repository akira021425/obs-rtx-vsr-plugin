import os

os.system("git commit -a -m \"Fix Cuda colorspace conversion\"")
os.system("git tag 1.0.5")
os.system("git push origin main")
os.system("git push origin 1.0.5")
