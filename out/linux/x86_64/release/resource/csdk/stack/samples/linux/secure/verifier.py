import sys
f=open(sys.argv[1],'r')
l=f.readlines()
i=-1
d={}
while (i<len(l)-1):
	i+=2
	s=l[i].split(' ')
	#print s;
	uri=s[1]
	method=s[4]
	result=s[7]
	if uri not in d:
		d[uri]=[[0,0,0],[0,0]]
	if method == "Get":
		d[uri][0][0]+=1
	elif method == "Post":
		d[uri][0][1]+=1
	else:
		d[uri][0][2]+=1
	if result == "Granted":
		d[uri][1][0]+=1
	else:
		d[uri][1][1]+=1
for i in d:
	print "uri:" + i
	print "get_requests:" + str(d[uri][0][0]),
	print "  post_requests:" + str(d[uri][0][1]),
	print "  put_requests:" + str(d[uri][0][2])
	print "granted_requests:" + str(d[uri][1][0]),
	print "  denied_requests:" + str(d[uri][1][1]),
	print "  grant_rate:"+ str(float(d[uri][1][0])/(d[uri][1][0]+d[uri][1][1])*100)+"%"

