from sds011lib import SDS011QueryReader

# Windows dùng COMx
sensor = SDS011QueryReader('COM3')   # <-- đổi đúng COM của bạn

while True:
    aqi = sensor.query()
    
    if aqi is not None:
        print(f"PM2.5 = {aqi.pm25} µg/m³")
        print(f"PM10  = {aqi.pm10} µg/m³")
        print("----------------------")