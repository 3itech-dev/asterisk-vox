# Модуль работы с событиями


`WaitEventInit()` - инициализация очереди событий
`WaitEvent([timeout])` - ожидание события (timeout в секундах)

После получения события устанавливаются переменные 
- `WAITEVENTSTATUS` - SUCCESS (если получено событие) или FAIL (если нет)
- `WAITEVENTFAILREASON` - если получен FAIL, то содержит описание ошибки (TIMEOUT - истекло время, HANGUP - повесили трубку, BAD_EVENT - внутренняя ошибка)
- `WAITEVENTNAME` - если SUCCESS - название события
- `WAITEVENTBODY` - если SUCCESS - описание событие


# Модуль TTS

`VoxPlayBackgroundInit([conf_fname],[endpoint])` - инициализация TTS
  conf_fname - имя конфигурационного файла (по умолчанию voxtts.conf
  endpoint - TTS grpc endpoint (по умолчанию берет из conf файла)
  
`VoxPlayBackground([&][COMMAND_NAME,OPTIONS,DATA])` - добавляет в очередь проигрывания команду
  Если перед название команды указан знак &, то команда добавляется в очередь
  Если не указан - то текущее воспроизведение прерывается и команда выполняется сразу
  Возможные команды:
  - `say,,INPUT` - произнести фразу, где INPUT - JSON вида {"text":"фраза которую нужно произнести"}
  - `sleep,,TIMEOUT` - сделать паузу в проигрывании (в секундах)
  
  При проигрывании генерируются следующие события:
  - `VoxPlayBackgroundFinished(0)` - проигрывание (выполнение команды) завершено
  - `VoxPlayBackgroundDuration(0,DURATION_SECS)` - перед началом проигрывание передает сколько оно будет длиться 
  - `VoxPlayBackgroundError(0)` - ошибка проигрывание
  
  
  `(0,DURATION_SECS`)` - означает что в WAITEVENTBODY будет через запятую записано число 0 и длительность в секундах
  
### Примеры 
  - `VoxPlayBackgroundInit();`
  - `VoxPlayBackground(say,,{"text":"приветствую вас на нашем канале"});`
  - `VoxPlayBackground(&say,,{"text":"слушаю вас"});`
  
  
# Модуль ASR

`VoxASRBackground([endpoint],,[model])` - запустить распознавание речи в фоне  
- `endpoint` - ASR grpc endpoint (по умолчанию берет из conf файла)
- `model` - используемая модель (по умолчанию берет из conf файла
	
`VoxASRBackgroundFinish()` - завершить распознавание	
	
При распознавании генерируются следующие события:
- `VOX_ASR_JSON_UTF8(JSON)` - полный результат распознавания фразы в виде JSON
- `VOX_ASR_JSON_ASCII(JSON)` - полный результат распознавания фразы (только ASCII символы - экранирования всех символов Unicode за пределами диапазона ASCII)
- `VOX_ASR_TEXT(TEXT)` - результат распознавания фразы - возвращает только текст
- `VOX_ASR_SESSION_FINISHED(STATUS,ERROR_CODE,ERROR_MESSAGE)` - завершение распознавания по ошибке или после вызова VoxASRBackgroundFinish

### Формат JSON:
```
 {
   "is_final": true/false,
   "transcript": "распознанный текст", // передается в VOX_ASR_TEXT
   "chunks": [
     {
	   "words": ["слово1", "слово2", ...],
	   "cofidence": 1.0,
	   "loudness": 1.0,
	   "start_time": {
	     "seconds": 10.0,
		 "nanos": 123457890
	   },
	   "end_time": {
	     "seconds": 11.0,
		 "nanos": 9876543210
	   },
	 },
	 ...
   ]
 }
```