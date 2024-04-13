# Run xv6

### 만약 Container가 완전히 꺼져있다면,
```
$ docker start ele3021
$ docker attach ele3021
```

### Container가 background에서 실행 중이라면,
```
$ docker attach ele3021
```

### Container가 꺼져있는지, background에서 실행 중인지 확인하려면
```
$ docker ps –a
```
라는 옵션 붙여서 실행시 확인 가능

### 실행

xv6-public 폴더 안에 들어가서
```
$ ./bootxv6.sh
```

---

### Extra Info
- docker는 sudo 쓰면 안된다
