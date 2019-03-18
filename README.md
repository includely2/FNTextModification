# FNTextModification
## 1. FNText (Original version)
### Description
[FNText: A Fast Neural Model for Efﬁcient Text Classiﬁcation](emnlp2018.pdf)  
Implementation from original author can be found in [here](https://github.com/Ra1nyHouse/FNText)  
Training set and testing set are examples from original author:
[Training set](src/ag.train.txt),
[Testing set](src/ag.text.txt)
### Structure
![fntext](fntext.jpg)  
[Source code](src/fntext_bi.c)
### Results
According to original author, it is suitable to set embedding dim = 400, epoch = 10 and batch size = 500 for small dataset.
Trained with parameter setting above, the model reaches a accuracy of 93.03205% on the testing set.
## 2. FNText with positional embedding
### Description
Inspired by [Attention Is All You Need](http://papers.nips.cc/paper/7181-attention-is-all-you-need), FNText is modified with positional embedding. Positional embedding vectors are directly added with word embedding vectors to construct the final vectors which are fed into the model.  
![fn](fn.JPG)  
  
Four types of positional embedding have been implemented
* Fixed positional embedding(PE)  
![pe_ori](pe_ori.JPG)
![pe_mod](pe_mod.JPG)
* Learned positional embedding
* Positional embedding with fixed parameter  
![fn_la](fn_la.JPG)  
* Positional embedding with learned parameter
### Structure
![fntext_mod](fntext_mod.jpg)
### Results
dim = 400, epoch = 10, batch size = 500

Model|Acc(%)|
-|-|
Origin|93.03205
Fixed PE|92.64052
Learned PE|93.01638
Learned PE with lambda=0.5|93.23684
Learned PE with Lambda=0.2|93.23030
Learned PE with lambda=0.1|93.28421
Learned PE with Lambda=0.05|93.22105
Learned PE with Lambda=0.02|93.13816
Learned PE with Lambda=0.01|93.17500
Learned PE with Learned lambda|93.28974

## 3. FNText_max with positional embedding
### Description
* Replacing the average pooling with max pooling
### Structure
![fntext_max](fntext_max.jpg)
### Results
Acc(%)|200|300|400|500|600
-|-|-|-|-|-|
5|
10|
15|
20|
