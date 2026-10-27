path='src/internal/main.cpp'
text=open(path,encoding='utf-8').read()
old='''  bool locked = fsLock(500);
  if (!locked) {
    Serial.println("[LOG] ФС занята, сброс часа отложен");
    return;
  }
  if (!LittleFS.exists(LOGS_DIR))
    LittleFS.mkdir(LOGS_DIR);

  bool wrote = false;'''
new='''  // Если окно уже слито ранее — просто очищаем буферы, чтобы retry больше не дублировал
  if (pendingWinId != 0 && pendingWinId == lastFlushedWinId) {
    hourCount    = 0;
    hourWindows  = 0;
    extHourCount = 0;
    pendingWinId = 0;
    return;
  }

  bool locked = fsLock(500);
  if (!locked) {
    Serial.println("[LOG] ФС занята, сброс часа отложен");
    return;
  }
  if (!LittleFS.exists(LOGS_DIR))
    LittleFS.mkdir(LOGS_DIR);

  // Повторная проверка под блокировкой
  if (pendingWinId != 0 && pendingWinId == lastFlushedWinId) {
    hourCount    = 0;
    hourWindows  = 0;
    extHourCount = 0;
    pendingWinId = 0;
    fsUnlock();
    return;
  }

  bool wroteMain = false;
  bool wroteExt  = false;'''
if old not in text:
    print('NOT FOUND 1')
    raise SystemExit(1)
text=text.replace(old,new)

old2='''      hourCount   = 0;
      hourWindows = 0;
      wrote       = true;
    } else {
      Serial.println("[LOG] ошибка открытия файла, сброс отложен");
    }
  } else {
    hourWindows = 0; // по основному буферу писать нечего
  }

  if (extHourCount > 0) {
    String path    = extTodayFile();
    bool   isNew   = !LittleFS.exists(path);
    File   f       = LittleFS.open(path, "a");
    if (f) {
      size_t written = 0;
      if (isNew) {
        int n = f.println("timestamp,datetime,temperature,humidity,pressure,batt");
        if (n > 0)
          written += (size_t)n;
      }
      for (uint8_t i = 0; i < extHourCount; i++) {
        int n = f.printf("%lu,%s,%.2f,%.2f,%.1f,%.2f\n", extHourBuf[i].ts,
                          fmtTime(extHourBuf[i].ts).c_str(), extHourBuf[i].t, extHourBuf[i].h,
                          extHourBuf[i].p, extHourBuf[i].b);
        if (n > 0)
          written += (size_t)n;
      }
      f.close();
      cacheAddPath(path, written, true);
      Serial.printf("[LOG] сброс буфера ext: %u строк в %s\n", extHourCount, path.c_str());
      extHourCount = 0;
      wrote        = true;
    }
  }

  if (wrote)
    lastLogMs = millis();

  rotateLogsIfNeeded();
  fsUnlock();'''
new2='''      hourCount    = 0;
      hourWindows  = 0;
      wroteMain    = true;
    } else {
      Serial.println("[LOG] ошибка открытия файла, сброс отложен");
    }
  } else {
    hourWindows = 0; // по основному буферу писать нечего
  }

  if (extHourCount > 0) {
    String path    = extTodayFile();
    bool   isNew   = !LittleFS.exists(path);
    File   f       = LittleFS.open(path, "a");
    if (f) {
      size_t written = 0;
      if (isNew) {
        int n = f.println("timestamp,datetime,temperature,humidity,pressure,batt");
        if (n > 0)
          written += (size_t)n;
      }
      for (uint8_t i = 0; i < extHourCount; i++) {
        int n = f.printf("%lu,%s,%.2f,%.2f,%.1f,%.2f\n", extHourBuf[i].ts,
                          fmtTime(extHourBuf[i].ts).c_str(), extHourBuf[i].t, extHourBuf[i].h,
                          extHourBuf[i].p, extHourBuf[i].b);
        if (n > 0)
          written += (size_t)n;
      }
      f.close();
      cacheAddPath(path, written, true);
      Serial.printf("[LOG] сброс буфера ext: %u строк в %s\n", extHourCount, path.c_str());
      extHourCount = 0;
      wroteExt     = true;
    }
  }

  if (wroteMain || wroteExt) {
    if (pendingWinId != 0)
      lastFlushedWinId = pendingWinId;
    pendingWinId = 0;
    lastLogMs    = millis();
  } else if (pendingWinId != 0) {
    // Нечего писать, но окно закрыто — помечаем как обработанное, чтобы не дублировать
    lastFlushedWinId = pendingWinId;
    pendingWinId     = 0;
  }

  rotateLogsIfNeeded();
  fsUnlock();'''
if old2 not in text:
    print('NOT FOUND 2')
    raise SystemExit(1)
text=text.replace(old2,new2)
open(path,'w',encoding='utf-8').write(text)
print('done')
