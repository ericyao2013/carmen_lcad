
import numpy as np


def normalize_theta(theta):
  if theta >= -np.pi and theta < np.pi:
    return theta
  
  multiplier = np.floor(theta / (2 * np.pi))
  theta = theta - multiplier * 2 * np.pi
  
  if theta >= np.pi:
    theta -= 2 * np.pi

  if theta < -np.pi:
    theta += 2 * np.pi

  return theta


def rotate(x, y, angle):
    rot_x = np.cos(angle) * x - np.sin(angle) * y
    rot_y = np.sin(angle) * x + np.cos(angle) * y
    return rot_x, rot_y


class Transform2d:
    def __init__(self, x = 0, y = 0, th = 0):
        self.x = x
        self.y = y
        self.th = th 

    def transform(self, t):
        result = Transform2d(self.x, self.y, self.th)
        result.transform_inline(t)
        return result
    
    def transform_inline(self, t):
        rx, ry = rotate(t.x, t.y, self.th)
        self.x = self.x + rx
        self.y = self.y + ry
        self.th = normalize_theta(self.th + t.th)
    
    def inverse(self):
        result = Transform2d()
        result.x, result.y = rotate(-self.x, -self.y, -self.th)
        result.th = -self.th
        return result

    def __repr__(self):
        return '[%f, %f, %f]' % (self.x, self.y, self.th) 

if __name__ == '__main__':
    print(rotate(1, 0, np.deg2rad(45)))  # [1, 1]
    
    p1 = Transform2d(5, 5, 0)
    p2 = Transform2d(2, 2, 0)
    print(p2.inverse().transform(p1))  # [3, 3]
    
    p2 = Transform2d(2, 2, np.deg2rad(45)) 
    print(p2.inverse().transform(p1))  # [3, 0]
